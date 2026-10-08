"""Offline CLI regression: no scan, device open, or flash command is executed."""
import pathlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = str(pathlib.Path(sys.argv.pop(1)).resolve())


def image_bytes():
    payload = b'OpenRockCLI image fixture\x00\xff'
    outer, package = bytearray(102), bytearray(140 + 112)
    struct.pack_into('<IH', outer, 0, 0x57464b52, 102)
    struct.pack_into('<II', outer, 33, 102, len(package) + len(payload))
    struct.pack_into('<I', package, 0, 0x46414b52)
    package[8:12] = b'Test'
    struct.pack_into('<I', package, 136, 1)
    package[140:144] = b'boot'
    package[172:180] = b'boot.img'
    struct.pack_into('<IIII', package, 236, len(package), 64, 1, len(payload))
    return outer + package + payload, payload


class ImageCliTests(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([CLI, *map(str, args)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=15)

    def test_chinese_paths_and_spaces(self):
        with tempfile.TemporaryDirectory(prefix='openrockcli-') as temp:
            directory = pathlib.Path(temp) / '\u4e2d\u6587 \u56fa\u4ef6'
            directory.mkdir()
            image = directory / '\u5347\u7ea7 update.img'
            content, payload = image_bytes()
            image.write_bytes(content)
            result = self.run_cli('inspect', image)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output = directory / '\u89e3\u5305\u8f93\u51fa'
            result = self.run_cli('unpack', image, output)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((output / 'boot.img').read_bytes(), payload)

    def test_invalid_image_has_failure_exit(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'broken.img'
            image.write_bytes(b'not firmware')
            self.assertNotEqual(self.run_cli('inspect', image).returncode, 0)
            self.assertNotEqual(self.run_cli('inspect', image.with_name('missing.img')).returncode, 0)

    def test_rkaf_metadata_ranges(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'metadata.img'
            content, _ = image_bytes()
            for field, value in [(236, 0), (244, 0xffffffff)]:
                damaged = bytearray(content)
                struct.pack_into('<I', damaged, 102 + field, value)
                image.write_bytes(damaged)
                self.assertNotEqual(self.run_cli('inspect', image).returncode, 0)

    def test_unpack_rejects_windows_device_names_and_ads(self):
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'update.img'
            for name in ['NUL', 'COM1.bin', 'boot.img:stream', 'file.', 'file?']:
                content, _ = image_bytes()
                content[274:338] = name.encode().ljust(64, b'\0')
                image.write_bytes(content)
                self.assertNotEqual(self.run_cli('unpack', image, pathlib.Path(temp) / 'out').returncode, 0)

    def test_unpack_does_not_overwrite_existing_file(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = pathlib.Path(temp)
            image = directory / 'update.img'
            content, _ = image_bytes()
            image.write_bytes(content)
            output = directory / 'boot.img'
            output.write_bytes(b'preserve me')
            self.assertNotEqual(self.run_cli('unpack', image, directory).returncode, 0)
            self.assertEqual(output.read_bytes(), b'preserve me')

    def test_sparse_file_above_four_gib(self):
        # Exercises _wstat64 + 64-bit offsets without allocating a 4 GiB buffer.
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / 'large.img'
            content, _ = image_bytes()
            with image.open('wb') as stream:
                if os.name == 'nt':
                    import ctypes
                    import msvcrt
                    done = ctypes.c_ulong()
                    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                    ioctl = kernel.DeviceIoControl
                    ioctl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                      ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
                    ioctl.restype = ctypes.c_int
                    self.assertTrue(ioctl(msvcrt.get_osfhandle(stream.fileno()), 0x900c4,
                                          None, 0, None, 0, ctypes.byref(done), None))
                stream.write(content)
                stream.seek(0x100000010)
                stream.write(b'\0')
            result = self.run_cli('inspect', image)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
