import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    "runtime_probe", Path(__file__).resolve().parents[1] / "tools/pack_runtime_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class RuntimeProbeTests(unittest.TestCase):
    def setUp(self):
        self.boot = bytearray(2048)
        self.boot[:8] = b"ANDROID!"
        struct.pack_into("<I", self.boot, 40, 2)
        self.payloads = {name: name.encode() * 3 for name, _, _ in probe.SLOTS}

    def test_payloads_and_commands_match_partition_geometry(self):
        image, records = probe.pack(self.boot, self.payloads)
        self.assertLessEqual(len(image), probe.PARTITION_BYTES)
        for record in records:
            offset, size = record["offset"], record["size"]
            self.assertEqual(image[offset:offset + size], self.payloads[record["name"]])
            if "command" in record:
                _, _, address, sector, count = record["command"].split()
                self.assertEqual((int(sector, 16) - probe.PARTITION_START) * 512, offset)
                self.assertGreaterEqual(int(count, 16) * 512, size)

    def test_rejects_overlapping_kernel(self):
        self.payloads["kernel"] = bytes((32 << 20) + 1)
        with self.assertRaises(ValueError):
            probe.pack(self.boot, self.payloads)

    def test_rejects_unknown_android_layout(self):
        struct.pack_into("<I", self.boot, 40, 3)
        with self.assertRaises(ValueError):
            probe.pack(self.boot, self.payloads)

    def test_rejects_empty_initrd(self):
        self.payloads["initrd"] = b""
        with self.assertRaises(ValueError):
            probe.pack(self.boot, self.payloads)


if __name__ == "__main__":
    unittest.main()
