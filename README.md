# OpenRockCLI

A command-line and lightweight TUI firmware flashing tool for Rockchip SoCs.
It keeps low-level Rockchip maskrom/loader commands and adds an
OpenixCLI-compatible workflow for scanning devices and flashing Rockchip
`update.img` firmware packages.

## Features

- Device scanning with USB bus/port, mode, VID/PID, and chip name
- OpenixCLI-compatible commands: `scan`, `flash`, `tui`
- Rockchip firmware helper commands: `devices`, `inspect`, `unpack`
- Native Rockchip `update.img` inspection, extraction, and flashing
- Embedded MiniLoader download from Maskrom and automatic Loader reconnection
- Streaming partition writes with optional read-back verification
- Native primary/backup GPT generation from packaged Rockchip parameters
- Firmware info and extraction helpers
- Original low-level maskrom, storage, flash, OTP, and vendor storage commands

## Architecture

```mermaid
flowchart LR
    user[User / CI / Script] --> cli[openrockcli CLI]
    cli --> parser[Command parser and TUI]
    parser --> backend[Rockchip firmware backend]
    parser --> lowlevel[Low-level Rockusb commands]
    backend --> image[Native RKFW / RKAF parser]
    image --> stream[Partition stream writer]
    stream --> lowlevel
    lowlevel --> transport[USB transport]
    transport --> libusb[Linux/macOS: libusb-1.0]
    transport --> native[Windows: rockusb.sys]
    native --> device[Rockchip device in Loader or Maskrom]
    libusb --> device
```

The OpenixCLI-style commands (`scan`, `flash`, `devices`, `inspect`, `unpack`,
and `tui`) live in the CLI layer. Firmware parsing and flashing are implemented
inside OpenRockCLI. Linux/macOS use libusb; Windows talks directly to the
manufacturer `rockusb.sys` driver through Win32 file I/O. No external Rockchip
flashing program is required.

## Source Layout

```text
openrockcli/
├── .github/workflows/      GitHub Actions build and release workflow
├── include/                Public and internal C headers
├── payloads/               Maskrom helper payload sources
├── src/                    C implementation files
├── 99-openrockcli.rules    Linux udev rules
├── Makefile                Native Linux build
├── Makefile.win            MinGW Windows build
└── README.md
```

## How to build

### Linux platform

OpenRockCLI depends on the `libusb-1.0` library. Install `libusb-1.0-0-dev`
before compiling, for example in Ubuntu:

```shell
sudo apt install libusb-1.0-0-dev
```

For Ubuntu users, release builds also provide an `amd64` `.deb` package:

```shell
sudo apt install ./openrockcli_*_amd64.deb
openrockcli --version
```

Then just type `make` at the root directory, you will see a binary program.

```shell
cd openrockcli
make
sudo make install
```

### Windows platform

Windows builds use the native `rockusb.sys` backend. Keep the manufacturer's
Rockusb driver installed; do not replace it with WinUSB/Zadig for this build.
No libusb DLL, vendor CLI, vendor DLL, or `config.ini` is required.

Cross-compile on Linux with MinGW-w64:

```shell
make -f Makefile.win -j4 test-build
```

Or build and run offline tests in MSYS2 UCRT64 (GCC, make, Python):

```shell
make -j4 test NAME=openrockcli.exe
```

To create a Windows x64 installer from the built executable, install NSIS 3
with `amd64-unicode` support and run:

```shell
python3 packaging/windows/build-installer.py
```

The installer and SHA-256 checksum are written to `.build/artifacts/`. It
installs for the current user, adds desktop/Start menu command-window shortcuts
and an uninstaller, and keeps the manufacturer's USB driver in place. The
installer supports Simplified Chinese and English. It is an unsigned native
backend development preview, with hardware validation still pending.

The 2026-10-08 full LYNX Windows candidate has completed a user-confirmed
A1/CM5 flash, with MCP recording successful device-checksum verification and
exit code 0. This qualifies that observed workflow, not every driver/Loader
version or the standalone CLI installer. Read
[Windows transport and validation](docs/WINDOWS_ROCKUSB.md) and the
[local source handoff](docs/LOCAL_HANDOFF.md) for the scope and evidence.

Windows `scan` reports bus `0` and the full Windows USB location path in the
Port column. Copy the entire path (quoted) into `--port`; old libusb bus/port
bindings are not translated or silently accepted. Rebind the saved LYNX USB
location from a new scan before flashing through LYNX.

## OpenixCLI-Compatible Usage

Show the OpenixCLI-style command surface:

```shell
openrockcli --help
openrockcli --version
openrockcli help flash
openrockcli help tui
```

Launch the interactive TUI:

```shell
openrockcli
openrockcli tui
```

Scan for Rockchip USB devices:

```shell
openrockcli scan
```

List Rockusb devices:

```shell
openrockcli devices
```

Inspect firmware package info:

```shell
openrockcli inspect update.img
```

Extract firmware package contents:

```shell
openrockcli unpack update.img ./extracted
```

Flash firmware:

```shell
openrockcli flash update.img [options]
```

### Flash Options

| Option | Short | Description |
|--------|-------|-------------|
| `--bus` | `-b` | USB bus selector shown by `scan` |
| `--port` | `-P` | USB port selector shown by `scan` |
| `--verify` | `-V` | Enable verification preference; also accepts `true` or `false` |
| `--no-verify` | | Disable verification preference |
| `--mode` | `-m` | `partition`, `keep_data`, `partition_erase`, or `full_erase` |
| `--partitions` | `-p` | Comma-separated partition names for `--mode partition` |
| `--post-action` | `-a` | `reboot`, `none`, `poweroff`, or `shutdown` |
| `--noreset` | | Leave the device in Loader mode (same as `--post-action none`) |
| `--verbose` | `-v` | Print selected options |

Examples:

```shell
openrockcli flash update.img --verbose
openrockcli flash update.img --post-action poweroff
openrockcli flash update.img --mode full_erase --verify true
openrockcli flash update.img --mode partition --partitions boot,uboot,misc,recovery
openrockcli flash update.img --mode partition --partitions rootfs,oem,userdata
```

Notes:

- OpenRockCLI parses the outer RKFW header and embedded RKAF package table
  directly. Partition locations come from the package table, so no SDK tool or
  shell process is involved.
- GPT firmware parameters are converted to primary and backup GPT tables;
  SPI NAND IDB copies are written before GPT so the partition table is not
  erased while preparing the boot reserve.
- In Maskrom mode, the packaged MiniLoader is downloaded and OpenRockCLI waits
  for the same physical USB port to reconnect in Loader mode.
- `--verify true` queries Loader capabilities before erasing. Full READ_LBA support
  selects host byte-for-byte readback. A Loader that restricts host readback but
  advertises LBAwrite Parity selects Rockchip's device-side media checksum instead:
  after every write, the Loader reads the media and validates the supplied 32-bit
  additive checksum. The log identifies `host-readback` or `device-checksum`.
  Device checksum is not a cryptographic hash or a byte-for-byte comparison.
  Capability, transfer or verification failures stop the task; there is no
  fallback after failure. A zero checksum would disable the vendor check, so that
  write request is rejected before any of its commands are sent.
- `--mode partition` writes only the comma-separated RKAF entries selected by
  `--partitions`; it does not extract the package to a temporary directory.
- `poweroff` and `shutdown` request Rockusb reset subcode `0x02` and require its
  acknowledgement; `reboot` requests a normal reset. `none`/`--noreset` sends no
  reset and leaves the device in Loader mode. Physical power behaviour depends
  on the board's Loader.
- For raw sector operations, use the original low-level `openrockcli flash read`,
  `openrockcli flash write`, and `openrockcli flash erase` commands below.

## Low-level Usage

Low-level Rockchip commands are still available behind the OpenixCLI-style
entry points:

```shell
openrockcli help low-level
```

```shell
usage:
    openrockcli                              - Launch interactive TUI
    openrockcli tui                          - Launch interactive TUI
    openrockcli scan                         - List connected Rockchip USB devices
    openrockcli devices                      - List Rockusb devices
    openrockcli inspect <firmware.img>       - Inspect Rockchip firmware package
    openrockcli unpack <firmware.img> <dir>  - Unpack Rockchip firmware package
    openrockcli flash <firmware.img> [opts]  - Flash Rockchip update.img natively
    openrockcli maskrom <ddr> <usbplug> [--rc4-off]    - Initial chip using ddr and usbplug in maskrom mode
    openrockcli download <loader>                      - Initial chip using loader in maskrom mode
    openrockcli upgrade <loader>                       - Upgrade loader to flash in loader mode
    openrockcli ready                                  - Show chip ready or not
    openrockcli version                                - Show chip version
    openrockcli capability                             - Show capability information
    openrockcli reset [maskrom]                        - Reset chip to normal or maskrom mode
    openrockcli dump <address> <length>                - Dump memory region in hex format
    openrockcli read <address> <length> <file>         - Read memory to file
    openrockcli write <address> <file>                 - Write file to memory
    openrockcli exec <address> [dtb]                   - Call function address(Recommend to use extra command)
    openrockcli otp <length>                           - Dump otp memory in hex format
    openrockcli sn                                     - Read serial number
    openrockcli sn <string>                            - Write serial number
    openrockcli vs dump <index> <length> [type]        - Dump vendor storage in hex format
    openrockcli vs read <index> <length> <file> [type] - Read vendor storage
    openrockcli vs write <index> <file> [type]         - Write vendor storage
    openrockcli storage                                - Read storage media list
    openrockcli storage <index>                        - Switch storage media and show list
    openrockcli flash                                  - Detect flash and show information
    openrockcli flash erase <sector> <count>           - Erase flash sector
    openrockcli flash read <sector> <count> <file>     - Read flash sector to file
    openrockcli flash write <sector> <file>            - Write file to flash sector
extra:
    openrockcli extra maskrom --rc4 <on|off> [--sram <file> --delay <ms>] [--dram <file> --delay <ms>] [...]
    openrockcli extra maskrom-dump-arm32 --rc4 <on|off> --uart <register> <address> <length>
    openrockcli extra maskrom-dump-arm64 --rc4 <on|off> --uart <register> <address> <length>
    openrockcli extra maskrom-write-arm32 --rc4 <on|off> <address> <file>
    openrockcli extra maskrom-write-arm64 --rc4 <on|off> <address> <file>
    openrockcli extra maskrom-exec-arm32 --rc4 <on|off> <address>
    openrockcli extra maskrom-exec-arm64 --rc4 <on|off> <address>
```

## Tips

- The maskrom command can only used in maskrom mode, Before executing other commands, you must first execute the maskrom command

- The memory base address from 0, **NOT** sdram's physical address.

- In some u-boot rockusb driver, The flash dump operation be limited to the start of 32MB, you can patch u-boot's macro `RKUSB_READ_LIMIT_ADDR`.

```
diff --git a/u-boot/cmd/rockusb.c b/u-boot/cmd/rockusb.c
--- a/u-boot/cmd/rockusb.c
+++ b/u-boot/cmd/rockusb.c
@@ -26,7 +26,7 @@ static int rkusb_read_sector(struct ums *ums_dev,
        lbaint_t blkstart = start + ums_dev->start_sector;
        int ret;

-       if ((blkstart + blkcnt) > RKUSB_READ_LIMIT_ADDR) {
+       if ((blkstart + blkcnt) > RKUSB_READ_LIMIT_ADDR && 0) {
                memset(buf, 0xcc, blkcnt * SECTOR_SIZE);
                return blkcnt;
        } else {
```

### RV1106

```shell
openrockcli maskrom rv1106_ddr_924MHz_v1.15.bin rv1106_usbplug_v1.09.bin --rc4-off
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 off --sram rv1106_ddr_924MHz_v1.15.bin --delay 10 --rc4 off --dram rv1106_usbplug_v1.09.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 off --sram rv1106_ddr_924MHz_v1.15.bin --delay 10
```

- Dump memory region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm32 --rc4 off --uart 0xff4c0000 0xffff0000 1024
```

- Initial ddr memory and wirte `xstar.bin` to memory and jump to running

```shell
openrockcli extra maskrom --rc4 off --sram rv1106_ddr_924MHz_v1.15.bin --delay 10
openrockcli extra maskrom-write-arm32 --rc4 off 0x00000000 xstar.bin
openrockcli extra maskrom-exec-arm32 --rc4 off 0x00000000
```

### RK1808

```shell
openrockcli maskrom rk1808_ddr_933MHz_v1.05.bin rk1808_usbplug_v1.05.bin
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 on --sram rk1808_ddr_933MHz_v1.05.bin --delay 10 --rc4 on --dram rk1808_usbplug_v1.05.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 on --sram rk1808_ddr_933MHz_v1.05.bin --delay 10
```

- Dump bootrom region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm64 --rc4 on --uart 0xff550000 0xffff0000 1024
```

### RK3128

```shell
openrockcli maskrom rk3128_ddr_300MHz_v2.12.bin rk3128_usbplug_v2.63.bin
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 on --sram rk3128_ddr_300MHz_v2.12.bin --delay 10 --rc4 on --dram rk3128_usbplug_v2.63.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 on --sram rk3128_ddr_300MHz_v2.12.bin --delay 10
```

- Dump memory region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm32 --rc4 on --uart 0xff1a0000 0x60000000 1024
```

### RK3288

```shell
openrockcli maskrom rk3288_ddr_400MHz_v1.11.bin rk3288_usbplug_v2.63.bin
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 on --sram rk3288_ddr_400MHz_v1.11.bin --delay 10 --rc4 on --dram rk3288_usbplug_v2.63.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 on --sram rk3288_ddr_400MHz_v1.11.bin --delay 10
```

- Dump memory region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm32 --rc4 on --uart 0xff690000 0x60000000 1024
```

### RK3399

```shell
openrockcli maskrom rk3399_ddr_800MHz_v1.30.bin rk3399_usbplug_v1.30.bin
openrockcli version
```
```shell
openrockcli extra maskrom --rc4 on --sram rk3399_ddr_800MHz_v1.30.bin --delay 10 --rc4 on --dram rk3399_usbplug_v1.30.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 on --sram rk3399_ddr_800MHz_v1.30.bin --delay 10
```

- Dump rk3399 bootrom region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm64 --rc4 on --uart 0xff1a0000 0xfffd0000 1024
```

### RK3399PRO

```shell
openrockcli maskrom rk3399pro_ddr_666MHz_v1.25.bin rk3399pro_usbplug_v1.26.bin
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 on --sram rk3399pro_ddr_666MHz_v1.25.bin --delay 10 --rc4 on --dram rk3399pro_usbplug_v1.26.bin --delay 10
openrockcli version
```

### PX30

```shell
openrockcli maskrom px30_ddr_333MHz_v1.16.bin px30_usbplug_v1.31.bin
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 on --sram px30_ddr_333MHz_v1.16.bin --delay 10 --rc4 on --dram px30_usbplug_v1.31.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 on --sram px30_ddr_333MHz_v1.16.bin --delay 10
```

- Dump bootrom region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm64 --rc4 on --uart 0xff160000 0xffff0000 1024
```

### RK3308

```shell
openrockcli maskrom rk3308_ddr_589MHz_uart2_m1_v1.31.bin rk3308_usbplug_v1.27.bin
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 on --sram rk3308_ddr_589MHz_uart2_m1_v1.31.bin --delay 10 --rc4 on --dram rk3308_usbplug_v1.27.bin --delay 10
openrockcli version
```

### RK3566

```shell
openrockcli maskrom rk3566_ddr_1056MHz_v1.11.bin rk356x_usbplug_v1.13.bin --rc4-off
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 off --sram rk3566_ddr_1056MHz_v1.11.bin --delay 10 --rc4 off --dram rk356x_usbplug_v1.13.bin --delay 10
openrockcli version
```

### RK3568

```shell
openrockcli maskrom rk3568_ddr_1560MHz_v1.11.bin rk356x_usbplug_v1.13.bin --rc4-off
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 off --sram rk3568_ddr_1560MHz_v1.11.bin --delay 10 --rc4 off --dram rk356x_usbplug_v1.13.bin --delay 10
openrockcli version
```

### RK3588

```shell
openrockcli maskrom rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.16.bin rk3588_usbplug_v1.11.bin --rc4-off
openrockcli version
```

```shell\
openrockcli extra maskrom --rc4 off --sram rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.16.bin --delay 10 --rc4 off --dram rk3588_usbplug_v1.11.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 off --sram rk3588_ddr_lp4_2112MHz_lp5_2400MHz_v1.16.bin --delay 10
```

- Dump memory region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm64 --rc4 off --uart 0xfeb50000 0xffff0000 1024
```

### RK3562

```shell
openrockcli maskrom rk3562_ddr_1332MHz_v1.05.bin rk3562_usbplug_v1.04.bin --rc4-off
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 off --sram rk3562_ddr_1332MHz_v1.05.bin --delay 10 --rc4 off --dram rk3562_usbplug_v1.04.bin --delay 10
openrockcli version
```

### RK3576

```shell
openrockcli maskrom rk3576_ddr_lp4_2112MHz_lp5_2736MHz_v1.05.bin rk3576_usbplug_v1.02.bin --rc4-off
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 off --sram rk3576_ddr_lp4_2112MHz_lp5_2736MHz_v1.05.bin --delay 10 --rc4 off --dram rk3576_usbplug_v1.02.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 off --sram rk3576_ddr_lp4_2112MHz_lp5_2736MHz_v1.05.bin --delay 10
```

- Dump memory region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm64 --rc4 off --uart 0x2ad40000 0x3ff81000 1024
```

### RK3506

```shell
openrockcli maskrom rk3506b_ddr_750MHz_v1.04.bin rk3506_usbplug_v1.02.bin --rc4-off
openrockcli version
```

```shell
openrockcli extra maskrom --rc4 off --sram rk3506b_ddr_750MHz_v1.04.bin --delay 10 --rc4 off --dram rk3506_usbplug_v1.02.bin --delay 10
openrockcli version
```

- Initial ddr memory

```shell
openrockcli extra maskrom --rc4 off --sram rk3506b_ddr_750MHz_v1.04.bin --delay 10
```

- Dump memory region in hex format by debug uart

```shell
openrockcli extra maskrom-dump-arm32 --rc4 off --uart 0xff0a0000 0xff910000 1024
```

## Links

* [The rockchip loader binaries](https://github.com/rockchip-linux/rkbin)
* [The rockchip rkdeveloptool](https://github.com/rockchip-linux/rkdeveloptool)

## CI Release

GitHub Actions builds Linux and Windows x86_64 binaries on push, pull request, and manual dispatch.

```mermaid
flowchart LR
    push[Push / Pull Request] --> build[Build jobs]
    build --> linux[Linux x86_64 tar.gz]
    build --> windows[Windows x86_64 zip]
    linux --> artifacts[Workflow artifacts]
    windows --> artifacts
    tag[v* tag] --> release[GitHub Release]
    artifacts --> release
    permissions[Repository GITHUB_TOKEN] --> release
```

To publish a release:

1. Ensure GitHub Actions has `contents: write` permission for releases.
2. Push a version tag:

```shell
git tag v1.2.0
git push origin v1.2.0
```

The workflow uploads:

- `openrockcli-linux-x86_64.tar.gz`
- `openrockcli_<version>_amd64.deb`
- `openrockcli-windows-x86_64.zip`
- SHA256 files for release packages

## License

This library is free software; you can redistribute it and or modify it under the terms of the MIT license. See [MIT License](LICENSE) for details.

## Complete raw eMMC images (1.3.0)

`openrockcli inspect-raw disk.img --loader loader.bin` validates an aligned whole-disk
MBR/GPT image and a RAM Loader without scanning or opening USB. Extended MBR
partition chains, sparse Android images, update.img containers and compressed files
are not raw disks. Both GPT copies and entry CRCs must be valid; partitions must be
in bounds and not overlap. Images above 4 GiB use 64-bit file offsets.

`openrockcli flash disk.img --raw --loader loader.bin --storage emmc --mode full_erase --verify true --bus BUS --port PORT`

This native transaction downloads the external Loader only in Maskrom, reconnects
at the same USB location, requires current storage to be eMMC, checks capacity and
verification capabilities before erasing the user area, and writes from LBA 0 with
per-block verification. An already-running Loader is reused. External Loader bytes
are never written as IDB; Boot0/Boot1, GPT relocation and partition expansion are
not performed. The disk must already contain board-matched boot components.
Verification is mandatory; failures never retry, downgrade verification or reboot.
Post action defaults to reboot; `--post-action none` leaves Loader running.
LYNX handles gzip/xz decompression, immutable task snapshots and file hashes.
