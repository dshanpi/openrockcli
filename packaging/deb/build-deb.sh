#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${root_dir}"

version="${OPENROCKCLI_VERSION:-}"
if [ -z "${version}" ]; then
  if [ -n "${GITHUB_REF_NAME:-}" ]; then
    version="${GITHUB_REF_NAME#v}"
  else
    version="$(git describe --tags --always --dirty 2>/dev/null | sed 's/^v//')"
  fi
fi
version="${version:-1.2.0}"

pkg="openrockcli"
arch="amd64"
stage="dist/deb/${pkg}_${version}_${arch}"
deb="dist/${pkg}_${version}_${arch}.deb"

rm -rf "${stage}" "${deb}"
install -d \
  "${stage}/DEBIAN" \
  "${stage}/usr/bin" \
  "${stage}/usr/share/doc/${pkg}" \
  "${stage}/usr/share/licenses/${pkg}" \
  "${stage}/lib/udev/rules.d"

install -m 0755 openrockcli "${stage}/usr/bin/openrockcli"
install -m 0644 README.md "${stage}/usr/share/doc/${pkg}/README.md"
install -m 0644 LICENSE "${stage}/usr/share/licenses/${pkg}/LICENSE"
install -m 0644 99-openrockcli.rules "${stage}/lib/udev/rules.d/99-openrockcli.rules"

installed_size="$(du -ks "${stage}" | awk '{print $1}')"
cat > "${stage}/DEBIAN/control" <<EOF
Package: ${pkg}
Version: ${version}
Section: utils
Priority: optional
Architecture: ${arch}
Depends: libc6 (>= 2.31), libusb-1.0-0 (>= 2:1.0.23)
Maintainer: OpenRockCLI Maintainers <noreply@github.com>
Installed-Size: ${installed_size}
Homepage: https://github.com/dshanpi/openrockcli
Description: Rockchip firmware flashing CLI
 OpenRockCLI is a command-line and lightweight TUI firmware flashing tool for
 Rockchip SoCs. It supports Rockchip USB scanning, low-level Rockusb commands,
 and update.img workflows through Rockchip upgrade_tool when available.
EOF

cat > "${stage}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v udevadm >/dev/null 2>&1; then
  udevadm control --reload-rules || true
  udevadm trigger || true
fi
exit 0
EOF
chmod 0755 "${stage}/DEBIAN/postinst"

fakeroot dpkg-deb --build "${stage}" "${deb}"
dpkg-deb --info "${deb}"
