import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from inspect_packed_dtb import parse_fdt, render_value, inspect
from workspace_policy import outside_tina


def small_fdt():
    # Header + empty reservation map + root with 64-bit memory reg + strings.
    structure = struct.pack('>I', 1) + b'\0' * 4
    structure += struct.pack('>III', 3, 16, 0) + struct.pack('>4I', 0, 0x40000000, 0, 0x20000000)
    structure += struct.pack('>II', 2, 9)
    strings = b'reg\0'
    return struct.pack('>10I', 0xD00DFEED, 56 + len(structure) + len(strings), 56, 56 + len(structure), 40, 17, 16, 0, len(strings), len(structure)) + bytes(16) + structure + strings


class DtbTests(unittest.TestCase):
    def test_memory_cells_remain_numeric(self):
        nodes, _ = parse_fdt(small_fdt())
        self.assertEqual(render_value(nodes['/']['reg']), ['0x00000000','0x40000000','0x00000000','0x20000000'])

    def test_zero_cell_is_not_empty_strings(self):
        self.assertEqual(render_value(bytes(4)), ['0x00000000'])

    def test_string_list(self):
        self.assertEqual(render_value(b'allwinner,t736\0arm,sun60iw2p1\0'), ['allwinner,t736','arm,sun60iw2p1'])

    def test_truncated_fdt_rejected(self):
        with self.assertRaises(ValueError):
            parse_fdt(small_fdt()[:-1])

    def test_missing_end_rejected(self):
        blob = bytearray(small_fdt())
        struct.pack_into('>I', blob, len(blob) - 8, 4)
        with self.assertRaises(ValueError):
            parse_fdt(blob)

    def test_hash_mismatch_writes_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            (p/'boot.img').write_bytes(small_fdt())
            with self.assertRaises(ValueError):
                inspect(p/'boot.img', '0'*64, p/'out')
            self.assertFalse((p/'out').exists())


class WorkspaceTests(unittest.TestCase):
    def test_tina_child_rejected(self):
        with self.assertRaises(ValueError):
            outside_tina(Path(tempfile.gettempdir())/'tina'/'mainline')

    def test_sdk_markers_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'device/config/chips').mkdir(parents=True)
            (root/'build').mkdir()
            (root/'build/envsetup.sh').write_text('')
            with self.assertRaises(ValueError):
                outside_tina(root/'new-mainline')

    def test_separate_sibling_allowed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'tina').mkdir()
            self.assertEqual(outside_tina(root/'t736-mainline'), (root/'t736-mainline').resolve())


if __name__ == '__main__':
    unittest.main()
