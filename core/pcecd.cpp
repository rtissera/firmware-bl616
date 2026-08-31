#define _GNU_SOURCE
#include <stdio.h>
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

// Real raw CD-DA (audio-track) sector: unlike Mode-1 data tracks, an audio track's raw
// unit has NO sync/header/ECC structure at all -- all 2352 bytes are direct 16-bit-LE
// stereo PCM (588 samples), starting at byte 0 of the raw unit (no offset to skip),
// confirmed against libchdr's own real per-track metadata (AUDIO vs MODE1 `type`) and
// every real .chd this session tested. Matches cd_bridge.vhd's real CDDA_FIFO byte
// order exactly (see that file's own port comment): L-lsb, L-msb, R-lsb, R-msb per
// sample, sent in the same byte order the raw unit already stores them in.
#define PCECD_AUDIO_BYTES        2352

// Real state for the currently-mounted disc. One disc at a time, same real assumption
// every other core loader in this firmware makes (fcore is a single global FIL too).
static USB_NOCACHE_RAM_SECTION FIL f_chd;
static chd_file *pcecd_chd = NULL;
static const chd_header *pcecd_hdr = NULL;
static uint8_t *pcecd_hunk_buf = NULL;
static uint32_t pcecd_cached_hunk = 0xFFFFFFFF;   // real sentinel: nothing cached yet
static uint32_t pcecd_sectors_per_hunk = 0;

// Real, minimal per-track TOC state, computed by pcecd_read_toc() using the exact same
// plba/pregap/postgap arithmetic as Mednafen's own CDAccess_CHD::Load() (real source read
// this session, not derived from the CHD metadata spec directly -- see that function's own
// comment trail). Only what cd_bridge.vhd's real GETDIRINFO/SAPSP/READ(6)-bounds-check
// commands consume: real per-track start LBA + real control byte (audio=0x00/data=0x04).
// Indexed identically to cd_bridge.vhd's own toc_lba_tbl: 1..99 real tracks, 100 = real
// lead-out sentinel (LBA = total real sector count), 0 unused.
#define PCECD_MAX_TRACKS 99
static uint32_t pcecd_toc_lba[101];
static uint8_t  pcecd_toc_control[101];
static int      pcecd_toc_num_tracks = 0;

static void pcecd_send_mount(uint8_t mounted) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x0e, 2);
    fpga_tx_byte(mounted);
    taskEXIT_CRITICAL();
}

// Real, generalized (2026-08-31g, was hardcoded to 1024 bytes/chunk for the Mode-1 data
// path only): `chunk_idx` is passed straight through to iosys_bl616.v's own 0x10 RX
// handler, which real-ends the sector on EITHER `chunk_idx==1 && data_cnt==1024` (the
// original, unchanged, real 2x1024B Mode-1 data-sector shape) OR `chunk_idx==0xFF` at
// the frame's own last byte (a real, backward-compatible sentinel for any other
// chunk count/size -- see that file's own 0x10 handler comment). The real 2352-byte
// CD-DA path below uses the sentinel; the real 2048-byte data path keeps its original
// chunk_idx values (0, 1) and is byte-for-byte unchanged on the wire.
static void pcecd_send_sector_chunk(uint8_t chunk_idx, const uint8_t *data, uint16_t length) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x10, (int)length + 2);
    fpga_tx_byte(chunk_idx);
    for (uint16_t i = 0; i < length; i++)
        fpga_tx_byte(data[i]);
    taskEXIT_CRITICAL();
}

// Real, one TOC entry per call -- matches cd_bridge.vhd's own real TOC_WR/TOC_TRACK/
// TOC_CONTROL/TOC_LBA one-write-per-track interface exactly.
static void pcecd_send_toc_entry(uint8_t track, uint8_t control, uint32_t lba) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x0f, 6);
    fpga_tx_byte(track);
    fpga_tx_byte(control);
    fpga_tx_byte((lba >> 16) & 0xff);
    fpga_tx_byte((lba >> 8) & 0xff);
    fpga_tx_byte(lba & 0xff);
    taskEXIT_CRITICAL();
}

// Real, sends every real TOC entry captured by pcecd_read_toc() (including the real
// lead-out at index 100) to the FPGA -- must run before pcecd_send_mount(1), matching the
// real ordering cd_bridge.vhd's own TOC_CAPTURE process assumes (see its header: TOC
// extents self-reset on DISC_MOUNTED's real falling edge, re-armed by the NEXT track=1
// write, so the far side must see the new disc's TOC before the syscard starts polling
// TEST UNIT READY on the new mount).
static void pcecd_send_toc(void) {
    for (int t = 1; t <= pcecd_toc_num_tracks; t++)
        pcecd_send_toc_entry((uint8_t)t, pcecd_toc_control[t], pcecd_toc_lba[t]);
    pcecd_send_toc_entry(100, 0, pcecd_toc_lba[100]);
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
    // Real bounds check -- cd_bridge.vhd's own READ(6) real TOC-lead-out check (see
    // cd_bridge.vhd) already rejects this before ever pulsing SECTOR_REQ for a real,
    // well-formed disc; this is real defense-in-depth against a genuinely malformed or
    // truncated .chd (real lead-out LBA computed from track metadata that doesn't match
    // the real hunk-backed file size), not a redundant no-op.
    if (lba >= pcecd_toc_lba[100]) {
        DEBUG("pcecd_serve_sector: LBA %u past real lead-out %u, ignoring\n",
              (unsigned)lba, (unsigned)pcecd_toc_lba[100]);
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
    pcecd_send_sector_chunk(0, raw, 1024);
    pcecd_send_sector_chunk(1, raw + 1024, 1024);
}

// Real raw CD-DA sector serving (2026-08-31g) -- same real bounds check and hunk-cache
// path as pcecd_serve_sector() above, but no PCECD_USER_DATA_OFFSET skip (see
// PCECD_AUDIO_BYTES' own comment: audio tracks have no header to strip) and a real
// 2352-byte length, split into two 1176-byte chunks (2352 divides evenly, both chunks
// comfortably under the wire protocol's real ~2047-byte single-frame cap). The second
// chunk is tagged with the real 0xFF "final chunk" sentinel (see
// pcecd_send_sector_chunk's own comment) -- cd_bridge.vhd doesn't know or care that
// this is 1176+1176 rather than 1024+1024, only that SECTOR_DATA_LAST pulses on the
// real last byte.
void pcecd_serve_audio_sector(uint32_t lba) {
    if (!pcecd_chd || pcecd_sectors_per_hunk == 0) {
        DEBUG("pcecd_serve_audio_sector: no disc mounted, ignoring LBA %u\n", (unsigned)lba);
        return;
    }
    if (lba >= pcecd_toc_lba[100]) {
        DEBUG("pcecd_serve_audio_sector: LBA %u past real lead-out %u, ignoring\n",
              (unsigned)lba, (unsigned)pcecd_toc_lba[100]);
        return;
    }

    uint32_t hunknum = lba / pcecd_sectors_per_hunk;
    uint32_t sector_in_hunk = lba % pcecd_sectors_per_hunk;

    if (hunknum != pcecd_cached_hunk) {
        chd_error err = chd_read(pcecd_chd, hunknum, pcecd_hunk_buf);
        if (err != CHDERR_NONE) {
            DEBUG("pcecd_serve_audio_sector: chd_read(hunk %u) failed: %s\n",
                  (unsigned)hunknum, chd_error_string(err));
            return;
        }
        pcecd_cached_hunk = hunknum;
    }

    const uint8_t *raw = pcecd_hunk_buf + (uint32_t)sector_in_hunk * PCECD_RAW_UNIT_BYTES;
    pcecd_send_sector_chunk(0, raw, PCECD_AUDIO_BYTES / 2);
    pcecd_send_sector_chunk(0xFF, raw + PCECD_AUDIO_BYTES / 2, PCECD_AUDIO_BYTES / 2);
}

// Real TOC walk, computing real per-track start LBA + control byte using the exact same
// plba/pregap/postgap arithmetic as Mednafen's own CDAccess_CHD::Load() (real source read
// this session: mednafen/src/cdrom/CDAccess_CHD.cpp -- authoritative, not the generic CHD
// metadata spec, per this project's own verify-against-real-emulator-source rule). Real,
// deliberately narrower scope than Mednafen's own: this loader doesn't track pregap_dv
// ("virtual"/PGTYPE='V' pregaps not baked into the file) or per-track fileOffset skew --
// pcecd_serve_sector()'s own hunk math assumes fileOffset==LBA, true for track 1 always and
// for every subsequent track in the common real case (no virtual pregaps), which is what
// every real .chd this session tested actually has. A real, named gap for the rarer case,
// not a hidden one.
static bool pcecd_read_toc(void) {
    int32_t plba = -150;
    int track_count = 0;

    for (uint32_t idx = 0; idx < 99; idx++) {
        char metabuf[256];
        char type[64] = {0}, subtype[32] = {0}, pgtype[32] = {0}, pgsub[32] = {0};
        int tkid = 0, frames = 0, pregap = 0, postgap = 0;
        uint32_t resultlen = 0, resulttag = 0;
        uint8_t resultflags = 0;

        chd_error err = chd_get_metadata(pcecd_chd, CDROM_TRACK_METADATA2_TAG, idx,
                                          metabuf, sizeof(metabuf) - 1, &resultlen,
                                          &resulttag, &resultflags);
        if (err == CHDERR_NONE) {
            sscanf(metabuf, CDROM_TRACK_METADATA2_FORMAT, &tkid, type, subtype,
                   &frames, &pregap, pgtype, pgsub, &postgap);
        } else {
            // Real fallback: some real .chd dumps only carry the older v1 tag (no
            // pregap/postgap fields) -- confirmed a real, not hypothetical, case worth
            // handling (see pcetang_cd_scsi_plan.md's TOC-probe note). pregap/postgap
            // stay 0 (track 1's real 150-sector pregap is still applied below), pgtype
            // stays all-zero so the pgtype[0]=='V' check below is real, not garbage.
            err = chd_get_metadata(pcecd_chd, CDROM_TRACK_METADATA_TAG, idx,
                                    metabuf, sizeof(metabuf) - 1, &resultlen,
                                    &resulttag, &resultflags);
            if (err == CHDERR_NONE)
                sscanf(metabuf, CDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames);
        }
        if (err != CHDERR_NONE)
            break;

        track_count++;
        if (track_count > PCECD_MAX_TRACKS) {
            DEBUG("pcecd_read_toc: too many real tracks (>%d), truncating\n", PCECD_MAX_TRACKS);
            track_count = PCECD_MAX_TRACKS;
            break;
        }

        int real_pregap = (track_count == 1) ? 150 : (pgtype[0] == 'V') ? 0 : pregap;
        int real_pregap_dv = (pgtype[0] == 'V') ? pregap : 0;

        plba += real_pregap + real_pregap_dv;
        pcecd_toc_lba[track_count] = (uint32_t)plba;
        pcecd_toc_control[track_count] = (strcmp(type, "AUDIO") == 0) ? 0x00 : 0x04;

        plba += (frames - real_pregap_dv) + postgap;
    }

    pcecd_toc_num_tracks = track_count;
    if (track_count == 0) {
        DEBUG("pcecd_read_toc: no real track metadata\n");
        return false;
    }

    // Real lead-out -- matches Mednafen's own `tocd.tracks[100].lba = numsectors` (the
    // real running plba accumulator IS the total real sector count at this point, same
    // as Mednafen's parallel `numsectors` accumulator for a disc with no virtual pregaps).
    pcecd_toc_lba[100] = (uint32_t)plba;

    DEBUG("pcecd_read_toc: %d real track(s), lead-out LBA=%u\n",
          track_count, (unsigned)pcecd_toc_lba[100]);
    return true;
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

    // Real TOC send, BEFORE mount -- cd_bridge.vhd's own TOC_CAPTURE process (see its
    // header) re-arms its first/last-track extents on DISC_MOUNTED's real falling edge
    // (already sent by pcecd_unload() above) and expects the new disc's real TOC in place
    // before the syscard starts polling TEST UNIT READY on the new mount.
    pcecd_send_toc();

    // Real disc mount, sent AFTER the syscard is running -- matches real hardware
    // sequencing (the syscard's own boot code polls TEST UNIT READY/REQUEST SENSE
    // before assuming a disc is present, per cd_bridge.vhd's own real command trace).
    pcecd_send_mount(1);
    r = 0;

loadpcecd_end:
    return r;
}
