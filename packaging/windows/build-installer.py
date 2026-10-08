#!/usr/bin/env python3
"""Package an already-built Windows x64 binary using NSIS 3 (amd64 target)."""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=repo / 'openrockcli.exe')
    parser.add_argument('--output', type=Path, default=repo / '.build/artifacts' /
                        'OpenRockCLI-1.2.1-native-preview.1-windows-x64-setup.exe')
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    raw = binary.read_bytes()
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    if (raw[:2] != b'MZ' or raw[pe:pe + 4] != b'PE\0\0' or
            struct.unpack_from('<H', raw, pe + 4)[0] != 0x8664):
        parser.error('--binary must be a Windows x64 PE executable')
    compiler = shutil.which('makensis')
    if not compiler:
        parser.error('makensis (NSIS 3 with amd64-unicode support) is required')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='openrockcli-installer-') as temp:
        payload = Path(temp)
        (payload / 'docs').mkdir()
        shutil.copy2(binary, payload / 'openrockcli.exe')
        strip = shutil.which('x86_64-w64-mingw32-strip')
        if strip:
            subprocess.run([strip, str(payload / 'openrockcli.exe')], check=True)
        for name in ['README.md', 'LICENSE', 'docs/WINDOWS_ROCKUSB.md']:
            shutil.copy2(repo / name, payload / name)
        for name in ['OpenRockCLI.cmd', '使用说明.txt']:
            content = (repo / 'packaging/windows' / name).read_text(encoding='utf-8')
            (payload / name).write_bytes(content.replace('\n', '\r\n').encode('utf-8'))
        revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo,
                                           text=True).strip()
        changes = subprocess.check_output(['git', 'status', '--porcelain'], cwd=repo,
                                          text=True).strip()
        packaged_hash = hashlib.sha256((payload / 'openrockcli.exe').read_bytes()).hexdigest()
        (payload / 'BUILD-INFO.txt').write_text(
            'OpenRockCLI Windows x64 native development preview\n'
            f'Source revision: {revision}{" + local changes" if changes else ""}\n'
            f'Packaged executable SHA256: {packaged_hash}\n'
            'Hardware qualification: PENDING\n', encoding='utf-8')
        subprocess.run([compiler, '-V3', f'-DPAYLOAD={payload}', f'-DOUTPUT={output}',
                        str(repo / 'packaging/windows/installer.nsi')], check=True)
    checksum = hashlib.sha256(output.read_bytes()).hexdigest()
    Path(str(output) + '.sha256').write_text(f'{checksum}  {output.name}\n')
    print(f'Installer: {output}\nSHA256: {checksum}')


if __name__ == '__main__':
    main()
