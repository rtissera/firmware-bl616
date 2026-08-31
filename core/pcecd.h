#pragma once

// Real PC Engine CD-ROM (.chd) loader -- see pcetang_cd_scsi_plan.md (pcetang project
// memory) for the full real wire-protocol design this implements (0x0e mount/0x10 sector
// chunk/0x06 sector request), verified against 3 real .chd dumps before this was written.

extern int loadpcecd(const char *fname);

// Real sector-request handler, called from main.cpp's uart1_rx_task on a real 0x06 frame
// (cd_bridge.vhd's own SECTOR_REQ, relayed over UART by iosys_bl616.v). `lba` is a real
// absolute CD-ROM sector number.
extern void pcecd_serve_sector(uint32_t lba);
