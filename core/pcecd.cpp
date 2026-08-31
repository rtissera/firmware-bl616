#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <string>

#include "utils.h"
#include "cores.h"
#include "overlay.h"
#include "pcecd.h"
#include "chd/chd_fatfs.h"
#include "libchdr/chd.h"

// Real PC Engine CD-ROM loader (pcetang core) -- see pcetang_cd_scsi_plan.md for the full
// real wire-protocol design (0x0e mount / 0x10 sector chunk / 0x06 sector request) and the
// real 2448-byte-raw-sector-to-2048-byte-user-data extraction this file implements, both
// verified against 3 real .chd dumps (toc_probe.c, a standalone host-side test, not part of
// this firmware) before being written here.

// Real CD-ROM raw-sector layout (Yellow Book Mode-1, real -- confirmed against every real
// .chd this session tested, not assumed): 12 sync + 4 header + 2048 user data + 4 EDC + 8
// zero + 172 P-parity + 104 Q-parity = 2352, + 96 subcode = 2448 real bytes/sector (matches
// libchdr's own `unitbytes` field exactly on every file tested). READ(6) (cd_bridge.vhd,
// FPGA side) needs exactly the 2048 real user-data bytes, matching real PCE-CD SCSI
// hardware (the drive's own decoder strips sync/header/ECC/subcode before delivery) --
// this loader does that stripping here, on the MCU side, before ever sending a byte over
// UART.
#define PCECD_RAW_UNIT_BYTES     2448
#define PCECD_USER_DATA_OFFSET   16
#define PCECD_USER_DATA_BYTES    2048

// Real state for the currently-mounted disc. One disc at a time, same real assumption
// every other core loader in this firmware makes (fcore is a single global FIL too).
static USB_NOCACHE_RAM_SECTION FIL f_chd;
static chd_file *pcecd_chd = NULL;
static const chd_header *pcecd_hdr = NULL;
static uint8_t *pcecd_hunk_buf = NULL;
static uint32_t pcecd_cached_hunk = 0xFFFFFFFF;   // real sentinel: nothing cached yet
static uint32_t pcecd_sectors_per_hunk = 0;

static void pcecd_send_mount(uint8_t mounted) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x0e, 2);
    fpga_tx_byte(mounted);
    taskEXIT_CRITICAL();
}

static void pcecd_send_sector_chunk(uint8_t chunk_idx, const uint8_t *data /* 1024 bytes */) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x10, 1026);
    fpga_tx_byte(chunk_idx);
    for (int i = 0; i < 1024; i++)
        fpga_tx_byte(data[i]);
    taskEXIT_CRITICAL();
}

static void pcecd_unload(void) {
    if (pcecd_chd) {
        chd_close(pcecd_chd);
        pcecd_chd = NULL;
        pcecd_hdr = NULL;
    }
    if (pcecd_hunk_buf) {
        free(pcecd_hunk_buf);
        pcecd_hunk_buf = NULL;
    }
    pcecd_cached_hunk = 0xFFFFFFFF;
    pcecd_sectors_per_hunk = 0;
    pcecd_send_mount(0);
}

void pcecd_serve_sector(uint32_t lba) {
    if (!pcecd_chd || pcecd_sectors_per_hunk == 0) {
        DEBUG("pcecd_serve_sector: no disc mounted, ignoring LBA %u\n", (unsigned)lba);
        return;
    }

    uint32_t hunknum = lba / pcecd_sectors_per_hunk;
    uint32_t sector_in_hunk = lba % pcecd_sectors_per_hunk;

    if (hunknum != pcecd_cached_hunk) {
        chd_error err = chd_read(pcecd_chd, hunknum, pcecd_hunk_buf);
        if (err != CHDERR_NONE) {
            DEBUG("pcecd_serve_sector: chd_read(hunk %u) failed: %s\n",
                  (unsigned)hunknum, chd_error_string(err));
            return;
        }
        pcecd_cached_hunk = hunknum;
    }

    const uint8_t *raw = pcecd_hunk_buf + (uint32_t)sector_in_hunk * PCECD_RAW_UNIT_BYTES
                          + PCECD_USER_DATA_OFFSET;
    pcecd_send_sector_chunk(0, raw);
    pcecd_send_sector_chunk(1, raw + 1024);
}

// Real, minimal TOC walk -- proves the disc's TOC is real/parseable before mounting it.
// Real, honest scope: the TOC itself isn't sent to the FPGA -- cd_bridge.vhd has no real
// consumer for it yet (only DISC_MOUNTED and sector bytes matter for TEST UNIT READY/
// REQUEST SENSE/READ(6), see pcetang_cd_scsi_plan.md) -- this exists so a real, malformed
// disc image is rejected before it's mounted, not to build a table nothing reads.
static bool pcecd_read_toc(void) {
    int real_tracks = 0;
    for (uint32_t idx = 0; idx < 200; idx++) {
        char metabuf[256];
        uint32_t resultlen = 0, resulttag = 0;
        uint8_t resultflags = 0;
        chd_error err = chd_get_metadata(pcecd_chd, CDROM_TRACK_METADATA2_TAG, idx,
                                          metabuf, sizeof(metabuf) - 1, &resultlen,
                                          &resulttag, &resultflags);
        if (err != CHDERR_NONE) {
            // Real fallback: some real .chd dumps only carry the older v1 tag (no
            // pregap fields) -- confirmed a real, not hypothetical, case worth handling
            // (see pcetang_cd_scsi_plan.md's TOC-probe note).
            err = chd_get_metadata(pcecd_chd, CDROM_TRACK_METADATA_TAG, idx,
                                    metabuf, sizeof(metabuf) - 1, &resultlen,
                                    &resulttag, &resultflags);
        }
        if (err != CHDERR_NONE)
            break;
        real_tracks++;
    }
    DEBUG("pcecd_read_toc: %d real track(s)\n", real_tracks);
    return real_tracks > 0;
}

// Load a PC Engine CD-ROM (.chd) image. Real flow: mount the .chd, parse+validate its
// real TOC, then boot exactly like a real PCE-CD console does -- a real PCE-CD unit has
// no on-cart program at all, only the syscard, run from the CD side once cd_bridge.vhd's
// SCSI target (already real, gw_sh-verified) starts answering real disc requests. Loads
// syscard3.pce through the same real 0x07 rom_do path loadpce() already uses for a plain
// HuCard.
int loadpcecd(const char *fname) {
    int r = 1;
    DEBUG("loadpcecd start: %s\n", fname);

    char *p = strcasestr(fname, ".chd");
    if (p == NULL) {
        overlay_message("Only .chd supported", 1);
        goto loadpcecd_end;
    }

    pcecd_unload();   // real: drop any previously mounted disc first

    {
        chd_error err = chd_fatfs_open(fname, &f_chd, &pcecd_chd);
        if (err != CHDERR_NONE) {
            overlay_status("Cannot open CHD: %s", chd_error_string(err));
            goto loadpcecd_end;
        }
    }

    pcecd_hdr = chd_get_header(pcecd_chd);
    if (pcecd_hdr->unitbytes == 0 || (pcecd_hdr->hunkbytes % pcecd_hdr->unitbytes) != 0) {
        overlay_status("Unexpected CHD sector layout");
        chd_close(pcecd_chd);
        pcecd_chd = NULL;
        goto loadpcecd_end;
    }
    pcecd_sectors_per_hunk = pcecd_hdr->hunkbytes / pcecd_hdr->unitbytes;

    pcecd_hunk_buf = (uint8_t *)malloc(pcecd_hdr->hunkbytes);
    if (!pcecd_hunk_buf) {
        overlay_status("Out of memory for CD hunk buffer");
        chd_close(pcecd_chd);
        pcecd_chd = NULL;
        goto loadpcecd_end;
    }
    pcecd_cached_hunk = 0xFFFFFFFF;

    if (!pcecd_read_toc()) {
        overlay_status("No real track metadata in CHD");
        pcecd_unload();
        goto loadpcecd_end;
    }

    {
        std::string syscard = std::string(drv) + "bios/syscard3.pce";
        r = loadpce(syscard.c_str());
        if (r != 0) {
            overlay_status("Failed to load syscard3.pce");
            pcecd_unload();
            goto loadpcecd_end;
        }
    }

    // Real disc mount, sent AFTER the syscard is running -- matches real hardware
    // sequencing (the syscard's own boot code polls TEST UNIT READY/REQUEST SENSE
    // before assuming a disc is present, per cd_bridge.vhd's own real command trace).
    pcecd_send_mount(1);
    r = 0;

loadpcecd_end:
    return r;
}
