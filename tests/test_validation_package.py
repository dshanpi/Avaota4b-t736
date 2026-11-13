import importlib.util
import hashlib
from pathlib import Path
import struct
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'pack_validation', Path(__file__).parents[1] / 'tools/pack_boot_validation.py')
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


class SparseValidation(unittest.TestCase):
    def image(self):
        header = struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, 4096, 3, 3, 0)
        raw = struct.pack('<HHII', 0xcac1, 0, 1, 4108) + bytes(4096)
        fill = struct.pack('<HHII', 0xcac2, 0, 1, 16) + b'abcd'
        hole = struct.pack('<HHII', 0xcac3, 0, 1, 12)
        return header + raw + fill + hole

    def test_accept_all_vendor_chunk_types(self):
        pack.validate_sparse(self.image(), 3 * 4096)

    def test_reject_wrong_partition_size(self):
        with self.assertRaises(ValueError):
            pack.validate_sparse(self.image(), 2 * 4096)

    def test_reject_truncation_and_trailing_bytes(self):
        for image in [self.image()[:-1], self.image() + b'\0']:
            with self.subTest(length=len(image)), self.assertRaises(ValueError):
                pack.validate_sparse(image, 3 * 4096)

    def test_reject_unknown_chunk_and_length_mismatch(self):
        for field, value, fmt in [(28, 0xcac4, '<H'), (36, 4107, '<I'), (16, 4, '<I')]:
            image = bytearray(self.image())
            struct.pack_into(fmt, image, field, value)
            with self.subTest(field=field), self.assertRaises(ValueError):
                pack.validate_sparse(image, 3 * 4096)

    def test_vendor_checksum_little_endian_padding_and_overflow(self):
        self.assertEqual(pack.checksum(b'\xff' * 4 + b'\x02'), b'\x01\0\0\0')
        self.assertEqual(pack.checksum(b'\x01\x02\x03'), b'\x01\x02\x03\0')


class LayoutValidation(unittest.TestCase):
    def setUp(self):
        self.payloads = {'sunxi_gpt.fex': b'known-gpt',
                         'sunxi_mbr.fex': b'known-mbr'}
        self.hashes = {name: hashlib.sha256(data).hexdigest()
                       for name, data in self.payloads.items()}
        self.payloads['sys_partition.fex'] = b''.join(
            f'[partition]\nname = {name}\nsize = {size}\n'.encode()
            for name, size in pack.VALIDATED_PARTITION_SIZES.items())
        self.lock = mock.patch.dict(pack.VALIDATED_LAYOUT_HASHES, self.hashes, clear=True)
        self.lock.start()
        self.addCleanup(self.lock.stop)

    def test_same_layout_is_accepted(self):
        pack.validate_layout(self.payloads)

    def test_changed_binary_partition_tables_are_rejected(self):
        for name in self.hashes:
            with self.subTest(name=name), self.assertRaises(ValueError):
                pack.validate_layout({**self.payloads, name: b'different-layout'})

    def test_changed_rootfs_size_is_rejected_even_with_same_gpt(self):
        self.payloads['sys_partition.fex'] = self.payloads['sys_partition.fex'].replace(
            b'size = 15239680', b'size = 2097152')
        with self.assertRaises(ValueError):
            pack.validate_layout(self.payloads)

    def test_duplicate_partition_is_rejected(self):
        self.payloads['sys_partition.fex'] += b'[partition]\nname = rootfs\nsize = 15239680\n'
        with self.assertRaises(ValueError):
            pack.validate_layout(self.payloads)


if __name__ == '__main__':
    unittest.main()
