# Native Windows RockUSB source handoff — 2026-10-08

The user requested local organization after confirming a successful flash.
Keep commits local: no push, remote PR, release, artifact upload or updater
promotion is authorized by this handoff. Runtime code remains byte-identical
to the source manifests for the successful full LYNX candidate; only acceptance
and handoff documentation has been added since that build.

## Code map

| Area | Files | Responsibility |
| --- | --- | --- |
| Native Windows access | `src/usb_windows.c`, `include/rockusb_windows_abi.h` | SetupAPI identity/location, wide paths, control handle, PIPE00 read/PIPE01 write, overlapped lifetime, actual Win32 errors |
| Shared transport | `include/usb_transport.h`, `src/usb_transport.c`, `src/usb_libusb.c`, `include/rock.h` | Device selection/transfer bounds, Linux/macOS libusb preserved, Maskrom bulk readiness |
| Protocol and verification | `src/rock.c`, `src/rockchip_backend.c` | Command/status validation, capability negotiation, full readback or advertised device checksum, IDB/GPT/partition workflow, no-reset and real poweroff |
| Image and path safety | `src/loader.c`, `src/rkimage.c`, `src/file_portable.c`, `include/file_portable.h`, `src/misc.c`, `include/x.h` | Loader/table/range checks, UTF-16 lengths, Unicode file IO, extraction safety, defined integer operations |
| CLI | `src/main.c`, `include/rockchip_backend.h`, `src/rockchip_usb_error.h` | Wide argv, complete physical location, true scan/flash exit status and diagnostics |
| Regression coverage | `tests/*` | Fake Win32 transport, USB/CSW, slow Maskrom, 58.2 GiB sparse model, checksum errors/zero sentinel, image/unpack/Unicode |
| Build/package | `Makefile`, `Makefile.win`, `.github/workflows/build-release.yml`, `packaging/windows/*` | Native Windows builds without libusb dependency; optional standalone CLI package, separate from the delivered full LYNX installer |

## Observed hardware result

The user's A1/CM5 full LYNX flash and MCP task report success, exit 0,
`device-checksum`, capability `15 17 00 00 00 00 00 00`, and successful reboot
command. LYNX holds the scoped receipt in
`docs/acceptance/evidence/REQ-REL-044_SUCCESS.json` and the consolidated handoff
in `docs/acceptance/WINDOWS_ROCKUSB_HANDOFF.md`.

Delivered OpenRockCLI executable SHA-256:

`872e0870e9edae9f225eddceb37dc2294b7967d26ee6bff1bdcc69247845ca0a`

The exe was built before these local commits from the same runtime source.
Do not infer that the former build's base commit alone includes this patch.
The matching full installer is `LYNX_0.9.4_x64-rockusb-verify-20261008-setup.exe`.

## Verification and reproduction

- Existing delivery evidence: Linux 4 C suites + 6 CLI, Windows/Wine 5 C suites +
  6 CLI, ASan/UBSan full simulated workflow all passed. New pre-fix regression
  had 20 failing assertions. Test doubles do not establish every driver version.
- This organization changes no C/header/test/Makefile runtime input. Its source
  hashes were compared against the delivered build before committing.
- Linux: `make -j4 test`.
- Windows cross-build: `make -f Makefile.win -j4 test-build`.
- Native Windows/MSYS2: `make -j4 test NAME=openrockcli.exe`.
- LYNX must stage the built `openrockcli.exe` and LICENSE in its existing sidecar
  resource directory, then perform a production Tauri Windows build. Preserve
  its other backends' libusb runtime; this native CLI does not require it.

Standalone NSIS installer qualification, additional boards/Loader versions,
update/signature acceptance and independent OS boot checks remain outside this
observed hardware success. No firmware rerun or remote publication is part of
this source cleanup.
