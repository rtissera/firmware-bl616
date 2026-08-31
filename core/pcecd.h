#pragma once

// Real PC Engine CD-ROM (.chd) loader -- see pcetang_cd_scsi_plan.md (pcetang project
// memory) for the full real wire-protocol design this implements (0x0e mount/0x10 sector
// chunk/0x06 sector request), verified against 3 real .chd dumps before this was written.

extern int loadpcecd(const char *fname);

// Real sector-request handlers, called from main.cpp's uart1_rx_task on a real 0x06 frame
// (cd_bridge.vhd's own SECTOR_REQ, relayed over UART by iosys_bl616.v). `lba` is a real
// absolute CD-ROM sector number. The frame's own is_audio bit (2026-08-31g, real
// SECTOR_IS_AUDIO tag) selects which one main.cpp calls: pcecd_serve_sector() answers
// with a real 2048-byte Mode-1 data sector, pcecd_serve_audio_sector() with a real
// 2352-byte raw CD-DA sector (direct 16-bit-LE stereo PCM, no header to strip).
extern void pcecd_serve_sector(uint32_t lba);
extern void pcecd_serve_audio_sector(uint32_t lba);
