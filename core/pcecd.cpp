#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include "FreeRTOS.h"
#include "task.h"

#include "tc_utils.h"
#include "cores.h"
#include "overlay.h"
#include "pcecd.h"
#include "chd/chd_fatfs.h"
#include "libchdr/chd.h"

extern void file_log(const char *msg);   // TEMP diagnostic, defined in main.cpp

// TEMP diagnostic (2026-09-10): largest single block newlib's heap will still hand out.
// Total free is the wrong question -- chd_open asks for a few sizeable contiguous blocks
// (codec instances, then the hunk buffer), so fragmentation, not the total, is what would
// bite. Probe downward and free immediately; costs nothing outside the log lines.
extern "C" void chd_dbg_log(const char *m) { file_log(m); }

// TEMP diagnostic (2026-09-10). The SDK's weak hooks are `printf(); while(1);` on UART0,
// which nothing is capturing here -- so both failures present as a silent hang with no
// reset, which is exactly the symptom being chased. Override them to leave a durable
// mark on the SD card first. Writing to FatFs from the overflow hook is not strictly
// safe (it can run from the scheduler's context), but at that point the system is
// already dead; a best-effort line costs nothing and names the failure.
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    char buf[64];
    snprintf(buf, sizeof(buf), "FATAL: stack overflow in task '%.24s'", pcTaskName ? pcTaskName : "?");
    file_log(buf);
    for (;;) {}
}

extern "C" void vApplicationMallocFailedHook(void) {
    file_log("FATAL: malloc failed (pvPortMalloc)");
    for (;;) {}
}

extern "C" unsigned chd_dbg_stack_free_bytes(void) {
    return (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
}

extern "C" size_t chd_dbg_largest_free_block(void) {
    // Bounded and coarse on purpose. The first version walked 512KB down in 1KB steps,
    // i.e. up to 512 failing mallocs, each of which can drive newlib into _sbrk; that
    // made the probe itself a plausible hang and cost three hardware rounds of wrong
    // conclusions. 32 steps of 8KB, ceiling 256KB.
    for (size_t sz = 256u * 1024u; sz >= 8u * 1024u; sz -= 8u * 1024u) {
        void *p = malloc(sz);
        if (p) { free(p); return sz; }
    }
    return 0;
}

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
// Logical LBA -> file frame. A CHD stores tracks back to back, each padded up to a
// multiple of CD_TRACK_PADDING (4) frames, and a pregap that is not in the file occupies
// LBAs but no bytes -- so file offset only equals LBA for a disc whose track 1 starts at
// LBA 0 with nothing skipped. Dungeon Explorer II is not one: track 1 is AUDIO (3365
// frames, padded to 3368) and track 2 carries a 225-frame pregap, putting the data track
// 222 frames apart in the two address spaces. The deltas differ per track (222, 371,
// 368 ... 545), so a single constant will not do.
// Accumulation follows beetle-pce-fast's CDAccess_CHD.cpp verbatim, including the
// postgap term my own derivation had missed, and its read mapping is the same:
//     file_frame = fileOffset[t] + (lba - LBA[t])
// Model checked against the real image before being written: summed padded frames came
// to 315468 against the file's own 39434 hunks x 8 = 315472, i.e. inside one hunk.
static uint32_t pcecd_toc_fofs[101];      // file frame where each track's LBA starts
static uint32_t pcecd_toc_sectors[101];   // playable sectors, for the track lookup
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

bool pcecd_is_mounted(void) {
    return pcecd_chd != NULL;
}

void pcecd_unload(void) {
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

// TEMP instrumentation (2026-09-10). The syscard boots and shows "JUST A MOMENT...",
// then sits there. The hot path below only logs on ERROR, so "no errors in the log" and
// "no sector request ever arrived" are indistinguishable -- which is exactly the gap that
// made the earlier hang take four rounds. These counters make the difference visible:
// zero requests means the fault is on the FPGA/cd_bridge side and nothing on the MCU is
// being asked for; a rising count with a slow rate means decode/IO throughput.
static uint32_t pcecd_req_count = 0;      // sector requests received
static uint32_t pcecd_hunk_reads = 0;     // actual chd_read() calls (cache misses)
static uint32_t pcecd_last_lba = 0;
static uint32_t pcecd_next_report = 1;

// 2026-09-11: the failure paths below used DEBUG(), which goes to dprint -> UART0, and
// NOTHING captures UART0 on this board. So a silent return looked identical to a request
// that was served fine, and the first real sector request stalled with no explanation.
// These now reach the SD log. They are one-shot (a stalled boot repeats nothing), so they
// cannot flood the UART RX path the way a per-sector log would.
static uint8_t pcecd_fail_logged = 0;
static uint8_t pcecd_served_logged = 0;
static uint8_t pcecd_decode_logged = 0;
static void pcecd_log_once(const char *msg) {
    if (pcecd_fail_logged) return;
    pcecd_fail_logged = 1;
    file_log(msg);
}

static void pcecd_progress_tick(void) {
    // Log on a 1,2,4,8,... schedule: dense at the start where "did anything happen at
    // all" is the question, then rare, so a working stream cannot flood the SD card
    // (file_log f_syncs every line).
    if (pcecd_req_count < pcecd_next_report)
        return;
    pcecd_next_report *= 2;
    char buf[112];
    snprintf(buf, sizeof(buf), "cdprog: reqs=%lu hunk_reads=%lu last_lba=%lu tick=%lu",
             (unsigned long)pcecd_req_count, (unsigned long)pcecd_hunk_reads,
             (unsigned long)pcecd_last_lba, (unsigned long)xTaskGetTickCount());
    file_log(buf);
}

// Logical LBA -> file frame, per beetle-pce-fast: find the track containing this LBA,
// then file = fileOffset[t] + (lba - LBA[t]). Linear over <=99 tracks, called once per
// sector request, which is nothing next to a hunk decode. Returns false if the LBA falls
// in no track's playable extent (a gap), so the caller declines rather than serving
// whatever happens to sit at that file frame.
static bool pcecd_lba_to_file_frame(uint32_t lba, uint32_t *out) {
    for (int t = 1; t <= pcecd_toc_num_tracks; t++) {
        uint32_t start = pcecd_toc_lba[t];
        if (lba >= start && lba < start + pcecd_toc_sectors[t]) {
            *out = pcecd_toc_fofs[t] + (lba - start);
            return true;
        }
    }
    return false;
}

void pcecd_serve_sector(uint32_t lba) {
    pcecd_req_count++; pcecd_last_lba = lba; pcecd_progress_tick();
    if (!pcecd_chd || pcecd_sectors_per_hunk == 0) {
        pcecd_log_once("SERVE-FAIL: no disc mounted (pcecd_chd NULL or spq 0)");
        return;
    }
    // Real bounds check -- cd_bridge.vhd's own READ(6) real TOC-lead-out check (see
    // cd_bridge.vhd) already rejects this before ever pulsing SECTOR_REQ for a real,
    // well-formed disc; this is real defense-in-depth against a genuinely malformed or
    // truncated .chd (real lead-out LBA computed from track metadata that doesn't match
    // the real hunk-backed file size), not a redundant no-op.
    if (lba >= pcecd_toc_lba[100]) {
        char b[80];
        snprintf(b, sizeof(b), "SERVE-FAIL: LBA %lu past lead-out %lu",
                 (unsigned long)lba, (unsigned long)pcecd_toc_lba[100]);
        pcecd_log_once(b);
        return;
    }

    uint32_t fframe;
    if (!pcecd_lba_to_file_frame(lba, &fframe)) {
        char b[80];
        snprintf(b, sizeof(b), "SERVE-FAIL: LBA %lu in no track extent (ntracks=%d)",
                 (unsigned long)lba, pcecd_toc_num_tracks);
        pcecd_log_once(b);
        return;
    }
    uint32_t hunknum = fframe / pcecd_sectors_per_hunk;
    uint32_t sector_in_hunk = fframe % pcecd_sectors_per_hunk;

    if (hunknum != pcecd_cached_hunk) {
        // Announce the decode BEFORE running it. Without this, "returned early" and
        // "still inside chd_read" produce identical logs -- the same trap that made the
        // chd_open hang take three hardware rounds earlier today.
        if (!pcecd_decode_logged) {
            pcecd_decode_logged = 1;
            char b[136];
            // NO heap probe here. chd_dbg_largest_free_block() walks malloc down from
            // 256KB and has now broken three separate hardware runs: it was the original
            // "f_open hangs" that cost three rounds, and adding it to THIS line silently
            // removed the DECODE-START output that the previous build printed fine.
            // Calling a probe that allocates, from inside the UART RX path, during a CD
            // load, is simply not safe. If heap state is needed, sample it somewhere
            // idle -- not on the path being diagnosed.
            snprintf(b, sizeof(b), "DECODE-START: lba=%lu fframe=%lu hunk=%lu sec=%lu",
                     (unsigned long)lba, (unsigned long)fframe,
                     (unsigned long)hunknum, (unsigned long)sector_in_hunk);
            file_log(b);
        }
        chd_error err = chd_read(pcecd_chd, hunknum, pcecd_hunk_buf);
        if (err != CHDERR_NONE) {
            char b[96];
            snprintf(b, sizeof(b), "SERVE-FAIL: chd_read(hunk %lu) = %d (%s)",
                     (unsigned long)hunknum, (int)err, chd_error_string(err));
            pcecd_log_once(b);
            return;
        }
        pcecd_cached_hunk = hunknum;
        pcecd_hunk_reads++;
    }

    const uint8_t *raw = pcecd_hunk_buf + (uint32_t)sector_in_hunk * PCECD_RAW_UNIT_BYTES
                          + PCECD_USER_DATA_OFFSET;
    pcecd_send_sector_chunk(0, raw, 1024);
    pcecd_send_sector_chunk(1, raw + 1024, 1024);

    // One-shot proof that a sector was actually decoded AND sent, with the mapping that
    // produced it -- lba, the file frame it resolved to, the hunk, and the first bytes of
    // user data. For LBA 3590 on this disc the mapping should give file frame 3368, and a
    // Mode-1 data sector's user area starts with the disc's own content, not zeros.
    if (!pcecd_served_logged) {
        pcecd_served_logged = 1;
        char b[160];
        snprintf(b, sizeof(b),
                 "SERVED: lba=%lu -> fframe=%lu hunk=%lu sec=%lu data=%02x%02x%02x%02x stack_free=%u",
                 (unsigned long)lba, (unsigned long)fframe, (unsigned long)hunknum,
                 (unsigned long)sector_in_hunk, raw[0], raw[1], raw[2], raw[3],
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        file_log(b);
    }
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
    pcecd_req_count++; pcecd_last_lba = lba; pcecd_progress_tick();
    if (!pcecd_chd || pcecd_sectors_per_hunk == 0) {
        DEBUG("pcecd_serve_audio_sector: no disc mounted, ignoring LBA %u\n", (unsigned)lba);
        return;
    }
    if (lba >= pcecd_toc_lba[100]) {
        DEBUG("pcecd_serve_audio_sector: LBA %u past real lead-out %u, ignoring\n",
              (unsigned)lba, (unsigned)pcecd_toc_lba[100]);
        return;
    }

    uint32_t fframe;
    if (!pcecd_lba_to_file_frame(lba, &fframe)) {
        DEBUG("serve: LBA %u is in no track extent, ignoring\n", (unsigned)lba);
        return;
    }
    uint32_t hunknum = fframe / pcecd_sectors_per_hunk;
    uint32_t sector_in_hunk = fframe % pcecd_sectors_per_hunk;

    if (hunknum != pcecd_cached_hunk) {
        // Announce the decode BEFORE running it. Without this, "returned early" and
        // "still inside chd_read" produce identical logs -- the same trap that made the
        // chd_open hang take three hardware rounds earlier today.
        if (!pcecd_decode_logged) {
            pcecd_decode_logged = 1;
            char b[136];
            // NO heap probe here. chd_dbg_largest_free_block() walks malloc down from
            // 256KB and has now broken three separate hardware runs: it was the original
            // "f_open hangs" that cost three rounds, and adding it to THIS line silently
            // removed the DECODE-START output that the previous build printed fine.
            // Calling a probe that allocates, from inside the UART RX path, during a CD
            // load, is simply not safe. If heap state is needed, sample it somewhere
            // idle -- not on the path being diagnosed.
            snprintf(b, sizeof(b), "DECODE-START: lba=%lu fframe=%lu hunk=%lu sec=%lu",
                     (unsigned long)lba, (unsigned long)fframe,
                     (unsigned long)hunknum, (unsigned long)sector_in_hunk);
            file_log(b);
        }
        chd_error err = chd_read(pcecd_chd, hunknum, pcecd_hunk_buf);
        if (err != CHDERR_NONE) {
            DEBUG("pcecd_serve_audio_sector: chd_read(hunk %u) failed: %s\n",
                  (unsigned)hunknum, chd_error_string(err));
            return;
        }
        pcecd_cached_hunk = hunknum;
        pcecd_hunk_reads++;
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
    int32_t fofs = 0;      // running file frame offset, see pcecd_toc_fofs above
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

        // File-frame accumulator, in lockstep with the logical one above.
        fofs += real_pregap_dv;                 // a pregap that IS in the file
        pcecd_toc_fofs[track_count]    = (uint32_t)fofs;
        pcecd_toc_sectors[track_count] = (uint32_t)(frames - real_pregap_dv);
        fofs += (frames - real_pregap_dv);
        fofs += postgap;
        fofs += ((frames + 3) & ~3) - frames;   // pad each track up to 4 frames

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
    file_log("loadpcecd: start");

    char *p = strcasestr(fname, ".chd");
    if (p == NULL) {
        file_log("loadpcecd: not a .chd, abort");
        overlay_message("Only .chd supported", 1);
        goto loadpcecd_end;
    }

    pcecd_unload();   // real: drop any previously mounted disc first
    file_log("loadpcecd: pcecd_unload done");

    {
        chd_error err = chd_fatfs_open(fname, &f_chd, &pcecd_chd);
        char buf[96];
        snprintf(buf, sizeof(buf), "loadpcecd: chd_fatfs_open err=%d (%s)", (int)err, chd_error_string(err));
        file_log(buf);
        if (err != CHDERR_NONE) {
            overlay_status("Cannot open CHD: %s", chd_error_string(err));
            goto loadpcecd_end;
        }
    }

    pcecd_hdr = chd_get_header(pcecd_chd);
    if (pcecd_hdr->unitbytes == 0 || (pcecd_hdr->hunkbytes % pcecd_hdr->unitbytes) != 0) {
        file_log("loadpcecd: bad sector layout, abort");
        overlay_status("Unexpected CHD sector layout");
        chd_close(pcecd_chd);
        pcecd_chd = NULL;
        goto loadpcecd_end;
    }
    pcecd_sectors_per_hunk = pcecd_hdr->hunkbytes / pcecd_hdr->unitbytes;
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "loadpcecd: hunkbytes=%u unitbytes=%u sectors_per_hunk=%u",
            (unsigned)pcecd_hdr->hunkbytes, (unsigned)pcecd_hdr->unitbytes, (unsigned)pcecd_sectors_per_hunk);
        file_log(buf);
    }

    pcecd_hunk_buf = (uint8_t *)malloc(pcecd_hdr->hunkbytes);
    if (!pcecd_hunk_buf) {
        file_log("loadpcecd: malloc for hunk buf FAILED");
        overlay_status("Out of memory for CD hunk buffer");
        chd_close(pcecd_chd);
        pcecd_chd = NULL;
        goto loadpcecd_end;
    }
    file_log("loadpcecd: hunk buf malloc ok");
    pcecd_cached_hunk = 0xFFFFFFFF;

    if (!pcecd_read_toc()) {
        file_log("loadpcecd: pcecd_read_toc FAILED, abort");
        overlay_status("No real track metadata in CHD");
        pcecd_unload();
        goto loadpcecd_end;
    }
    file_log("loadpcecd: TOC read ok");

    {
        std::string syscard = std::string(drv) + "bios/syscard3.pce";
        FILINFO fno;
        FRESULT sres = f_stat(syscard.c_str(), &fno);
        char buf[128];
        snprintf(buf, sizeof(buf), "loadpcecd: syscard=%s res=%d sz=%lu", syscard.c_str(), (int)sres,
            sres == FR_OK ? (unsigned long)fno.fsize : 0UL);
        file_log(buf);
        r = loadpce(syscard.c_str());
        snprintf(buf, sizeof(buf), "loadpcecd: loadpce(syscard) returned %d", r);
        file_log(buf);
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
    file_log("loadpcecd: TOC sent");

    // Real disc mount, sent AFTER the syscard is running -- matches real hardware
    // sequencing (the syscard's own boot code polls TEST UNIT READY/REQUEST SENSE
    // before assuming a disc is present, per cd_bridge.vhd's own real command trace).
    pcecd_send_mount(1);
    file_log("loadpcecd: mount sent, done");
    r = 0;

loadpcecd_end:
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "loadpcecd: returning %d", r);
        file_log(buf);
    }
    return r;
}
