# Licenses

This firmware is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)).

- Original TangCore BL616 firmware: copyright (c) 2025 nand2mario,
  [nand2mario/firmware-bl616](https://github.com/nand2mario/firmware-bl616), Apache-2.0.
- Changes and additions in this fork: copyright (c) 2026 Romain Tisserand, Apache-2.0.
  Files changed from the upstream version carry a "Modifications copyright" notice at
  the top, as Apache-2.0 section 4(b) requires.

## Third-party code in this tree

| Component | Location | License | License text |
|---|---|---|---|
| libchdr | `thirdparty/libchdr/` | BSD-3-Clause | `thirdparty/libchdr/LICENSE.txt`, file headers |
| LZMA SDK (decoder) | `thirdparty/libchdr/deps/lzma-26.02/` | Public domain | `thirdparty/libchdr/deps/lzma-26.02/LICENSE` |
| Zstandard 1.5.7 (decoder) | `thirdparty/libchdr/deps/zstd-1.5.7/` | BSD-3-Clause (dual BSD/GPLv2, BSD chosen) | `thirdparty/libchdr/deps/zstd-1.5.7/LICENSE` |
| miniz 3.1.2 | `thirdparty/libchdr/deps/miniz-3.1.2/` | MIT | header of `miniz.c` |
| dr_flac | `thirdparty/libchdr/include/dr_libs/dr_flac.h` | Public domain or MIT-0 | end of `dr_flac.h` |
| micro-flac | `thirdparty/micro-flac/` (git submodule, not vendored) | Apache-2.0 | `thirdparty/micro-flac/LICENSE` (present only after `git submodule update --init`) |
| CHD FatFs bridge | `chd/chd_fatfs.{c,h}` | BSD-3-Clause, (c) Romain Tisserand (from libchdr `contrib/tangcore-bl616`) | file headers |
| Mbed TLS sample config | `utils/mbedtls_sample_config.h` (copied unchanged from bouffalo_sdk examples) | Apache-2.0, (c) The Mbed TLS Contributors | file header |
| FreeRTOS config template | `FreeRTOSConfig.h` | MIT, (c) Amazon.com, Inc. | file header |
| FatFs config template | `fatfs_conf_user.h` | no license header; FatFs user-config template shipped with bouffalo_sdk | bouffalo_sdk |

## Derived code

- `fpga/programmer.cpp`: based in part on
  [openFPGALoader](https://github.com/trabucayre/openFPGALoader) by Gwenhael Goavec-Merou (Apache-2.0).
- `usb/hidparser.cpp`, `usb/usb_gamepad.cpp`: based on Till Harbaum's
  [FPGA-Companion](https://github.com/harbaum/FPGA-Companion) (Apache-2.0).
- `usb/cdc_debug.c`: descriptor code adapted from the bouffalo_sdk
  `usbd_cdc_acm` example (Apache-2.0).

The firmware links against [bouffalo_sdk](https://github.com/nand2mario/bouffalo_sdk)
(Apache-2.0, with its own bundled components such as FreeRTOS, FatFs, CherryUSB and
Mbed TLS), which is not part of this tree.
