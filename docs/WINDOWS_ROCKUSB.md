# Native Windows Rockusb backend

OpenRockCLI implements firmware parsing, loader download, Rockusb commands,
partition writes and readback verification itself. The Windows executable uses
the installed manufacturer's `rockusb.sys` driver. It does not spawn
`upgrade_tool.exe`, link libusb/WinUSB, or require a vendor DLL or `config.ini`.

## Status and scope

The x86_64 backend has been cross-compiled with MinGW-w64. Offline tests cover
protocol framing, Maskrom CRC/chunk boundaries, transfer failures, device
selection, verification failure and the no-reset option. Mocked Win32 tests
exercise the native transfer adapter. These tests do **not** demonstrate that
a real Windows driver accepts the requests or that a board boots afterward.

The four cross-compiled Windows test executables also passed under Wine 9.0,
as did CLI inspection/extraction with Chinese paths and spaces, invalid-image
failure exits, and inspection of a sparse image larger than 4 GiB. Wine does
not provide the manufacturer's kernel driver and is not a replacement for
the Windows hardware tests below. The Windows CI workflow is updated to run
the suites, but that hosted workflow has not been run for this local change.

Real Windows hardware acceptance is pending, including supported driver
versions, USB hubs, Maskrom re-enumeration and large firmware writes. This is a
development implementation, not a hardware-qualified release. No target board
was erased or flashed during this implementation.

## Driver interface evidence

The private driver ABI was checked against publicly available Rockchip
binaries, rather than inferred from generic USB control conventions:

* [Rockchip RKTools: DriverAssitant v4.5 x64/win8 rockusb.sys](https://github.com/rockchip-android/RKTools/blob/rk3399-box-7.1/windows/DriverAssitant_v4.5/Driver/x64/win8/rockusb.sys)
  SHA-256: `3c49eed237c6f2b7ec2820c9053e11b9ed6fa18b817ace1fe555aa5bbaf93a33`.
* [Rockchip RKTools: AndroidTool Release v2.43 archive](https://github.com/rockchip-android/RKTools/blob/rk3399-box-7.1/windows/AndroidTool/AndroidTool_Release_v2.43.zip).
  The extracted `AndroidTool.exe` SHA-256 is
  `04c55090d54f5b72f251fb21d9429f20cd3308e4e2619546663e6624a785b0e1`.

These names identify the distribution versions, not a compatibility guarantee
for every driver packaged with other versions of DriverAssistant.

| Operation | Native interface | Payload |
| --- | --- | --- |
| Loader command/data send | `WriteFile` on `device path + \PIPE01` | Existing Rockusb CBW/data bytes |
| Loader data/status receive | `ReadFile` on `device path + \PIPE00` | Existing Rockusb data/CSW bytes |
| Maskrom download index `0x471` | `DeviceIoControl(0x8000a000)` | Raw input bytes, no output buffer |
| Maskrom download index `0x472` | `DeviceIoControl(0x8000a004)` | Raw input bytes, no output buffer |

For the exact driver above, the device-control dispatch starts at virtual
address `0x140008a8c`; IOCTL comparisons are around `0x140008b00`, vendor URB
construction around `0x140008b43`, and returned transfer length around
`0x140008dcb`. The driver builds request `0x0c`, value zero, with index
`0x471`/`0x472`. It does not append the host's CRC. AndroidTool's download
routine at `0x490620`, notably the call setup at `0x490753`–`0x490769`, confirms
the raw input buffer, null output buffer, corresponding IOCTL and transfer
count. CRC, padding and 4096-byte download chunks remain in OpenRockCLI's
existing protocol implementation. Unrecognized download indexes are rejected.

## Enumeration and I/O

Enumeration uses SetupAPI's standard `GUID_DEVINTERFACE_USB_DEVICE`
(`a5dcbf10-6530-11d2-901f-00c04fb951ed`), Rockchip VID `2207`, and service
`Rockusb`. Composite child interfaces are excluded. The manufacturer's device
setup class GUID is not used as a device interface GUID.

The parent hub supplies the USB device descriptor through
`IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`; the descriptor's `bcdUSB`
convention and recognized loader product strings distinguish Loader from
Maskrom, matching the existing transport. The hub's standard interface GUID
is `f18a0e88-c30c-11d0-8815-00a0c906bed8`.

`scan` reports bus `0` and the complete first `SPDRP_LOCATION_PATHS` value as
the port. `flash` requires exactly one matching device, opens that location
again, and retains it while waiting for Maskrom-to-Loader re-enumeration.
Missing/ambiguous locations and unsupported drivers fail before flashing.
Multiple attached boards require an exact selector; an omitted selector is
accepted only when the scan finds exactly one device.

### Correction after the first Windows hardware report (2026-10-08)

The initial candidate incorrectly used the bare device handle for bulk I/O.
The operator reported `ERROR_INVALID_HANDLE` (Win32 6) on every 31-byte CBW
write, followed by `cannot detect target flash`. The original mocks accepted
bulk I/O on the control handle, so their passing result missed this defect.

AndroidTool 2.43's open routine at `0x4834c0` appends `\PIPE00` for reads
(`0x4835bc`, `GENERIC_READ`, `FILE_SHARE_READ`) and `\PIPE01` for writes
(`0x48356c`, `GENERIC_WRITE`, `FILE_SHARE_WRITE`). Its control branch
(`0x48353e`) opens the base path with read/write access and sharing. The
corrected adapter follows those paths and access modes. Completion queries,
cancellation and cancellation draining all use the handle of the submitted
operation. Maskrom opens only control; Loader opens both bulk pipes after
re-enumeration. Partial-open failures close every successfully opened handle.

A regression mock now rejects bulk I/O on the base handle with Win32 6. It
fails with the old adapter and passes with separate pipes; it also checks
Unicode paths, both directions, control routing, pending completions,
read/write cancellation, all three open-failure cleanup paths, idempotent close,
and Maskrom-to-Loader handle replacement. This is evidence of corrected
vendor ABI usage, **not** confirmation on the operator's installed driver.
The actual driver version and a successful Windows hardware run remain pending.

The target opens with `CreateFileW` and overlapped I/O, using the vendor's
sharing modes above. The handles do not establish cross-process exclusivity;
LYNX's existing hardware task guard remains necessary. Each
transfer waits up to 10 seconds before requesting cancellation. Cancellation
is drained before releasing the buffer and OVERLAPPED structure; therefore an
unresponsive kernel driver can extend this wait. Writes are not automatically
retried. Standard error includes the operation and numeric Win32 error;
standard output retains existing stage, progress and verification messages.

CLI arguments enter through `wmain`, become UTF-8 internally, and file access
uses wide Windows APIs. Image sizes use 64-bit stat/seek operations. Unicode
paths and paths containing spaces are covered by offline CLI tests; extended
Windows paths beyond normal platform limits are not separately qualified.

## Commands and integration

In PowerShell, after installing the manufacturer's Rockusb driver:

```powershell
.\openrockcli.exe scan
.\openrockcli.exe inspect 'C:\固件\update.img'
.\openrockcli.exe unpack 'C:\固件\update.img' 'C:\固件\解包'
```

For an intended target board, copy the full port from `scan` into the existing
`flash` command's `--bus 0 --port '...'` arguments. `--noreset` is equivalent
to `--post-action none`. Requested readback verification fails the task on a
mismatch, including all-`0xcc` data; it is never silently disabled.

LYNX can keep `flasherKind=openrockcli`. Its Windows launcher should use
`CreateProcessW`, capture both output streams and preserve the process exit
code. Remove its WinUSB/libusb prerequisite for this backend. Saved libusb
bus/port values must be rebound from a Windows native scan; they are not the
same location format. This repository change does not modify LYNX's launcher
or capability detection. No working-directory/config.ini dependency exists.

## Reproducible validation

```sh
make -j4 test                         # Linux regression suite
make -f Makefile.win -j4 test-build   # Windows x64 cross-build
```

On Windows, from MSYS2 UCRT64 with GCC, make and native UCRT64 Python installed:

```sh
make -j4 test NAME=openrockcli.exe
```

The Windows CI job runs the offline suites and rejects imports of libusb,
WinUSB or unexpected GCC runtime DLLs before packaging. Hardware acceptance
must separately cover single/multiple devices, exact location selection,
Maskrom loader download and re-enumeration, Loader flashing with readback,
`--noreset`, a Chinese image path, a mismatched verification result and an
interrupted connection. Keep the full logs, driver version, board/chip model,
image checksum and process exit code for those runs.

## Consolidated workflow audit (2026-10-08)

The next LYNX candidate combines the pipe fix with the REQ-REL-042 audit:

- READ_FLASH_INFO performs one read of up to 512 bytes and accepts 11..512,
  following [Rockchip RKComm.cpp](https://github.com/rockchip-linux/rkdeveloptool/blob/master/RKComm.cpp).
  It never read-exacts 512 bytes across the data/CSW boundary.
- Storage responses now validate CSW status; failed transport stops the workflow
  before additional probes or destructive requests. Common CSW failures log the
  opcode, status, signature and tag match. TestUnitReady's vendor-specific residue
  is not incorrectly forced to zero.
- IDB/GPT generation, target capacity/ranges/overlaps, erase extents and an empty
  partition selection are checked before erase. IDB, primary GPT and backup GPT
  verification all fail deterministically on mismatch, including all-0xcc data.
- MiniLoader validates all table entries, payload ranges and CRC before IDB
  construction; its UTF-16 name helper consumes a byte length correctly. The
  SPI NAND 256 KiB IDB slot cannot be overrun. Firmware RKAF allocation overflow,
  metadata overlap and duplicate partition names are rejected.
- Poweroff sends DEVICE_RESET subcode 2 and checks acknowledgement, per
  [Rockchip RKComm.h](https://github.com/rockchip-linux/rkdeveloptool/blob/master/RKComm.h).
  No-reset still sends no reset command; an unsupported poweroff cannot report
  success merely because the device stayed in Loader mode.
- Unpacking rejects Windows reserved names, ADS/special characters, duplicate
  and overlong output paths. Output creation is exclusive, so existing files,
  symlinks and hardlinks cannot be truncated by extraction.
- Unaligned 32-bit reads cast before shifting to avoid signed-shift UB.

`flash_audit_test` uses in-memory storage for complete eMMC, SPI NOR and SPI NAND
flows, Maskrom download/reopen, three independent verification failures, multiple
mid-command disconnects and poweroff rejection. Windows transport tests also
exercise the actual enumeration/open code through mocked SetupAPI and hub calls.
ASan/UBSan checks supplement the Linux and Wine suites. LYNX separately drains
its readers after process exit to avoid dropping late stderr diagnostics.

The candidate remains unqualified on the user's actual Windows driver. Image
checks cover structure/ranges, MiniLoader CRC and readback; they are not an
RKFW signature/authenticity verifier. Secure Boot, unknown extended image
formats and Windows extended-length paths require separate qualification.
MiniLoader is limited to 64 MiB and parameter to 1 MiB before erase.

### Loader verification negotiation (REQ-REL-044)

The full-image workflow now queries Loader capability bytes before erase. When
READ_LBA (bit 3) is enabled it retains strict host readback. When that bit is
absent and LBAwrite Parity (bit 10) is advertised, it instead selects the vendor's
on-device media checksum before any write. This is an explicit verification
method, logged as `device-checksum`, not byte comparison or a fallback after
failure. No supported verification method means no erase/write.

The protocol follows Rockchip's
[`rkusb_do_check_parity`](https://github.com/rockchip-linux/u-boot/blob/next-dev/drivers/usb/gadget/f_rockusb.c)
and its call after `do_write` in
[`f_mass_storage.c`](https://github.com/rockchip-linux/u-boot/blob/next-dev/drivers/usb/gadget/f_mass_storage.c):
checksum = 0x55aa + the little-endian 32-bit words, wrapping at 32 bits. CDB bytes
9..12 carry it in little-endian order, with cmdlen 16 so all checksum bytes reach
the Loader. The Loader reads media internally before returning CSW. Every write
chunk is at most 64 KiB; nonzero residue, wrong tag/signature, phase error, or I/O
failure aborts with no retry. An additive checksum is weaker than byte comparison
and is not cryptographic authentication. A checksum of zero disables the vendor
check; OpenRockCLI rejects such a request without transmitting any of its write
commands. Previous erase/writes are not rolled back.

Offline tests cover the reported 122142720-sector geometry using a sparse memory
fixture, restricted high-address readback, media corruption at IDB/GPT/partition,
CSW failures, zero-checksum sentinel and exact CDB encoding. Wine and these models
do not prove that the user's particular Loader advertises or implements parity;
real Windows/board acceptance remains pending.

### Observed success and local source handoff (2026-10-08)

After the final LYNX Windows candidate, the user confirmed successful flashing.
Read-only MCP telemetry independently reports `state=success`, `exitCode=0`,
`verifyState=success`, `verifyMethod=device-checksum`, and acknowledged reboot.
Loader capabilities are `15 17 00 00 00 00 00 00`: full READ_LBA bit 3 is clear,
and LBAwrite Parity bit 10 is set. This confirms the negotiated checksum path
was actually used on the A1/CM5 board. The previous failed readback bytes were
not captured, so do not retroactively label them measured all-0xcc data.

This is a single observed full LYNX workflow, not an exhaustive driver matrix,
standalone-installer acceptance or independent post-reboot OS boot validation.
Historical pending/failure entries above retain the evidence available at their
original delivery time. The user asked to organize source locally without
uploading; see [LOCAL_HANDOFF.md](LOCAL_HANDOFF.md).
