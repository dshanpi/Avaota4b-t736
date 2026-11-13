import base64
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from import_target_evidence import decode_capture, import_capture, memory_banks


def fdt(compatible=b"allwinner,t736\0", size=0x200000000):
    strings = bytearray()
    def prop(name, value):
        offset = len(strings)
        strings.extend(name.encode() + b"\0")
        return struct.pack(">III", 3, len(value), offset) + value + bytes((-len(value)) % 4)
    def node(name):
        value = name.encode() + b"\0"
        return struct.pack(">I", 1) + value + bytes((-len(value)) % 4)
    tree = node("") + prop("compatible", compatible)
    tree += prop("#address-cells", struct.pack(">I", 2))
    tree += prop("#size-cells", struct.pack(">I", 2))
    tree += node("memory@40000000") + prop("device_type", b"memory\0")
    tree += prop("reg", struct.pack(">QQ", 0x40000000, size))
    tree += struct.pack(">III", 2, 2, 9)
    return struct.pack(">10I", 0xd00dfeed, 56 + len(tree) + len(strings), 56, 56 + len(tree), 40, 17, 16, 0, len(strings), len(tree)) + bytes(16) + tree + strings


def capture(blob):
    return hashlib.sha256(blob).hexdigest() + "  /sys/firmware/fdt\nBEGIN_FDT_BASE64\n" + base64.encodebytes(blob).decode() + "END_FDT_BASE64\n"


class LiveEvidenceTests(unittest.TestCase):
    def test_valid_serial_crlf_and_64bit_memory(self):
        blob = fdt()
        decoded, nodes, _, _ = decode_capture(capture(blob).replace("\n", "\r\r\n"))
        self.assertEqual(blob, decoded)
        self.assertEqual(8 * 1024**3, memory_banks(nodes)[0]["size_bytes"])

    def test_corrupt_capture_rejected(self):
        text = capture(fdt()).replace("BEGIN_FDT_BASE64\n", "BEGIN_FDT_BASE64\n!")
        with self.assertRaisesRegex(ValueError, "base64"):
            decode_capture(text)

    def test_wrong_hash_rejected(self):
        text = capture(fdt())
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            decode_capture("0" * 64 + text[64:])

    def test_wrong_board_rejected(self):
        with self.assertRaisesRegex(ValueError, "expected T736"):
            decode_capture(capture(fdt(b"vendor,other-board\0")))

    def test_repeated_or_truncated_capture_rejected(self):
        text = capture(fdt())
        for bad in (text + text, text.replace("END_FDT_BASE64", "")):
            with self.assertRaisesRegex(ValueError, "one complete"):
                decode_capture(bad)

    def test_trailing_bytes_rejected(self):
        with self.assertRaisesRegex(ValueError, "trailing"):
            decode_capture(capture(fdt() + b"garbage"))

    def test_missing_hash_rejected(self):
        with self.assertRaisesRegex(ValueError, "SHA256"):
            decode_capture(capture(fdt()).split("\n", 1)[1])

    def test_different_evidence_cannot_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log = root / "serial.log"
            log.write_text(capture(fdt()), encoding="utf-8")
            import_capture(log, root / "out")
            original = (root / "out/effective.dtb").read_bytes()
            log.write_text(capture(fdt(size=0x20000000)), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "overwrite"):
                import_capture(log, root / "out")
            self.assertEqual(original, (root / "out/effective.dtb").read_bytes())


if __name__ == "__main__":
    unittest.main()
