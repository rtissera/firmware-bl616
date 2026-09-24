# TangCore firmware for BL616 — pcetang fork

This is a fork of [nand2mario/firmware-bl616](https://github.com/nand2mario/firmware-bl616),
the TangCore firmware that runs on the BL616 microcontroller of the Sipeed Tang boards.

**It is required for [pcetang](https://github.com/rtissera/pcetang)**, the PC Engine /
TurboGrafx-16 core. The stock TangCore firmware cannot load it: pcetang needs the CD-ROM²
loader (CHD images, sector and CD-audio streaming over the UART), backup-RAM saves, and a
PC Engine entry in the core menu. None of that is upstream yet.

**Report problems on the [pcetang issue tracker](https://github.com/rtissera/pcetang/issues)**
(issues are disabled here, to keep everything in one place).

Ko-fi and hardware donations are welcome to support this work: [ko-fi.com/rtissera](https://ko-fi.com/rtissera).

## Status

| | |
|---|---|
| Board | **Tang Console 60K only.** The PC Engine menu entry is shown on Console 60K only. Primer 25K and Nano 20K build, but loading games there needs an external MCU (work in progress). |
| PC Engine HuCards (`.pce`) | working |
| SuperGrafx (`.sgx`) | working (some titles may still be imperfect) |
| PC Engine CD (`.chd`) | working, including CD audio |
| Arcade Card CD games | boot and play, not perfect yet (graphics glitches in some titles, one known lock-up) |
| Backup-RAM saves | working, written to the SD card |
| Neo Geo | loader present but **hidden** — early preview, the core is not released |

## It may break other cores

This firmware changes code that every core goes through, not only PC Engine files:

- **UART link to the FPGA**: receive is interrupt-driven, transmit can use DMA, and every
  writer shares one lock.
- **JTAG bitstream loading** (`fpga/programmer.cpp`): fixes from the Console 60K bring-up.
- **USB gamepads** (`usb/hidparser.cpp`): 4-byte extended HID usages (Switch Pro and similar).
- **Menu and main loop** (`main.cpp`).

Tested on Console 60K with pcetang and mdtang. **Other cores are untested** with this
firmware. Until these changes are merged upstream, if another core misbehaves, go back to
the stock TangCore firmware for it.

Known: the **smstang** core built from its public repository does not answer this
firmware (nor current stock TangCore) — its `iosys_bl616.v` predates the length-prefixed
command protocol. That is a problem in smstang's public source, not in this firmware.

## Before you flash

- **Back up your BL616 flash first**, so you can return to stock. With the board in
  programming mode (hold BOOT, plug USB), from `bouffalo_sdk/tools/bflb_tools/bouffalo_flash_cube`:

  ```
  ./BLFlashCommand-ubuntu --interface uart --port /dev/ttyACM0 --chipname bl616 \
      --baudrate 2000000 --flash --read --start 0x0 --len 0x400000 --file backup.bin
  ```
- `flash_console60k_real.ini` writes **only** the TangCore image at `0x40000` and leaves
  Sipeed's partner firmware at `0x0` alone. `flash.ini` also rewrites `0x0` with the partner
  firmware, which you must download from Sipeed first.

## SD card layout (PC Engine)

```
cores/console60k/monitor.bin    TangCore menu bitstream
cores/console60k/pcetang.bin    the PC Engine core (fixed name)
bios/syscard3.pce               System Card 3.0 — required for CD games
pce/                            HuCards: .pce, .sgx
pcenginecd/                     CD games: .chd (one file per disc)
saves/pce/<game>.sav            created by the firmware (backup RAM)
```

The PC Engine entry opens the card root so you can reach both `pce/` and `pcenginecd/`.

**CD games must be CHD.** The firmware reads CD images only as `.chd` (MAME's compressed
format, with CD audio tracks inside the same file). `.bin/.cue`, `.iso` and `.img/.ccd` are
not supported. Convert with MAME's `chdman`:

```
chdman createcd -i "Game (Japan).cue" -o "Game (Japan).chd"
```

## Build gotchas

- Put the **T-Head toolchain** first on `PATH`. The Makefile's default `riscv64-unknown-elf-`
  otherwise picks up a distribution GCC, which fails with `unknown cpu 'e907'`.
- `rm -rf build` when switching boards or options: a stale `build/` gives misleading CMake
  errors that look like SDK bugs.
- `make TANG_BOARD=console60k` (default), `primer25k` or `nano20k`.
- `SHOW_NEOGEO=1` shows the Neo Geo entry (development only).
- `DEBUG=1` is a debug build: it writes `debug.log` to the SD card root (useful for bug
  reports). Release builds never write it.
- `USB_CDC_DEBUG=1` turns the USB port into a debug serial link — **USB gamepads and USB
  drives stop working** in that build. Never ship it.
- Builds are not byte-reproducible: the same source gives a different md5 each time.

## Releases

Tagged builds for all three boards are published by CI (`.github/workflows/release.yml`)
with an `MD5SUMS.txt`.

## How this was built

The pcetang changes were developed with AI assistance (Claude), under my direction, and
checked on real hardware before being claimed. The pcetang README describes how claims in
these two repositories are verified.

---

## Original README (upstream)


This is TangCore firmware for the on-board BL616 of Tang Console.

See [this document](https://github.com/nand2mario/tangcore/blob/main/doc/dev.md) for how the firmware works with cores.

## Build instructions

I'm building on Windows. Linux should also work. 

First download Bouffalo toolchain,

```bash
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_windows.git

# for Linux, clone: https://github.com/bouffalolab/toolchain_gcc_t-head_linux.git
```

Add `toolchain_gcc_t-head_linux/bin` to your path.

Then download a patched version of Bouffalo SDK.

```bash
git clone --recurse-submodules https://github.com/nand2mario/bouffalo_sdk.git

# point BL_SDK_BASE to its location
set BL_SDK_BASE=<sdk_dir>
```

Then it should build OK.

```
make
make flash COMX=com5
```

The 2nd line flashes the firmware to BL616 (The required `bl616_fpga_partner_60kConsole.bin` file is [here](https://dl.sipeed.com/shareURL/TANG/Console/09_MCU_FW)). Before executing that, press and hold the "BOOT" button on the Tang Console board (bottom left corner, close to one of the USB-C port), then plug in the USB cable. This enters the BL616 into the programming mode.

For the USB drive, You need an OTG dongle to turn the connector from a "device" one to a "host" one, and provide power at the same time.

Acknowledgements
* JTAG FPGA programming logic based on [openFPGALoader](https://github.com/trabucayre/openFPGALoader)
* Gamepad support based on Till Harbaum's [FPGA-Companion](https://github.com/harbaum/FPGA-Companion)

License
* Apache-2.0, see [LICENSE](LICENSE). Original firmware (c) 2025 nand2mario; changes in this fork (c) 2026 Romain Tisserand.
* Third-party code and its licenses: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
