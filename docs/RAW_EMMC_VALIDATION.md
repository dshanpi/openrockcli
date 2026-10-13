# Windows RK3576 raw eMMC validation — 2026-10-13

The user reported successful local flashing through the supplied LYNX 0.9.5
Windows x64 test installer and pasted the flasher output. This is user-reported
hardware evidence, not a separately observed agent run or a structured task export.
The runtime remains at OpenRockCLI `c081a0987c0b4dde02fe104894c7c3d8989b9368`
and LYNX `21f32663a5e17056326a61b7d9e79575e18d84f9`.
This documentation update does not change runtime code or the binary used.

The supplied installer was `LYNX_0.9.5_x64-rk-mainline-20261013-setup.exe`,
SHA-256 `af9eef305faa58290ea62141fc890cc38cac8ee2bcff465f24ed53dbd80508c5`.
This identifies the artifact supplied in the conversation; the user did not
independently provide installed executable hashes.

## Observed result

- RAM Loader components: `UsbHead`, `rk3576_ddr_lp4_2112M`, `rk3576_usbplug_v1`.
  RK3576 is inferred from these names; the exact board model was not provided.
- Loader download and reconnection completed before raw eMMC preflight.
- Capabilities: `15 17 00 00 00 00 00 00`; selected method: `device-checksum`.
- eMMC capacity: 122,142,720 sectors of 512 bytes.
- Raw image: 5,070,913,536 bytes, exceeding 4 GiB.
- User-area erase, write from LBA 0, and per-block verification reached 100%.
- Final output: `Flash complete! Verification: passed. Post action: reboot.`

Device-checksum means the Loader reads media after each write and checks the
Rockchip 32-bit additive checksum. It is neither host byte-for-byte readback nor
a cryptographic hash. The final line confirms the flasher accepted the reboot
command response; it does not show that the operating system booted.

## Preserved scope and outstanding evidence

Keep mandatory eMMC/capacity/verification preflight, the RAM-only external Loader,
full user-area erase, LBA0 writing and failure-without-retry/downgrade/reboot.
The implementation tests cover failure paths; this user run confirms a successful
path only. Existing `update.img` behavior remains separate.

The pasted output does not include task ID, structured exit code, saved board ID,
USB binding, source image filename/compression format or image/Loader hashes.
Do not infer those values or extend this result to all compression formats,
boards, Loaders, operating systems or drivers. Cold boot, OS startup, recovery,
clean installation/upgrade/uninstall and installed updater behavior remain
unverified. This record does not declare release readiness.

LYNX owns decompression and task snapshots. OpenRockCLI accepts an uncompressed,
sector-aligned complete MBR/GPT image, not a compressed image or update.img.
Only the eMMC user area is supported; Boot0/Boot1, GPT relocation and partition
expansion are outside this feature.
