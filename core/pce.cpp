#define _GNU_SOURCE
#include <string.h>      // for strcasestr

#include "utils.h"
#include "cores.h"
#include "overlay.h"

// Load a PC Engine / TurboGrafx-16 HuCard ROM (pcetang core)
// return 0 if successful
int loadpce(const char *fname) {
    unsigned int off = 0, br, total = 0;
    unsigned int size;
    int r = 1;
    DEBUG("loadpce start\n");

    // check extension .pce
    char *p = strcasestr(fname, ".pce");
    if (p == NULL) {
        overlay_message("Only .pce supported", 1);
        goto loadpce_end;
    }

    r = f_open(&fcore, fname, FA_READ);
    if (r) {
        overlay_status("Cannot open file");
        goto loadpce_end;
    }
    size = get_file_size(fname);

    // Some .pce dumps carry a 512-byte copier header ahead of the real HuCard
    // image (same convention real emulators use to detect it): a clean dump's
    // size is a whole multiple of 1KB, so a size that is off by exactly 512
    // bytes means a header is present. Skip it -- pcetang's own dynamic
    // ROM_SZ latch (pce_top.vhd) tracks byte count from whatever is streamed
    // here, so an unskipped header would silently shift and undersize the
    // whole image on the FPGA side.
    if ((size % 1024) == 512) {
        off = 512;
        size -= 512;
        DEBUG("loadpce: 512-byte copier header detected, skipping\n");
    }

    // load actual ROM
    set_loading_state(1);
    core_running = false;

    // Send rom content
    if ((r = f_lseek(&fcore, off)) != FR_OK) {
        overlay_status("Seek failure");
        goto loadpce_pce_end;
    }

    do {
        if ((r = f_read(&fcore, fbuf, 1024 /*BLOCK_SIZE*/, &br)) != FR_OK)
            break;
        // start rom loading command
        send_fbuf_data(br);
        taskYIELD();                // allow gamepad polling to run
        total += br;
        if ((total & 0xfff) == 0) {	// display progress every 4KB
            //              01234567890123456789012345678901
            overlay_status("%d/%dK                          ", total >> 10, size >> 10);
        }
    } while (br == 1024 /*BLOCK_SIZE*/ && total < size);

    DEBUG("loadpce: %d bytes\n", total);
    overlay_status("Success");
    core_running = true;

    overlay(0);		// turn off OSD

loadpce_pce_end:
    set_loading_state(0);   // turn off game loading, this starts the core
    f_close(&fcore);
loadpce_end:
    return r;
}
