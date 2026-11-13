import copy
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


sources = module("prepare_sources")
components = module("inventory_components")


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.lock = json.loads((ROOT / "config/sources.lock.json").read_text())

    def test_real_lock_valid(self):
        self.assertEqual(3, len(sources.validate_lock(self.lock)))

    def test_reject_vendor_or_shell_remote(self):
        for remote in ("https://github.com/radxa/u-boot.git", "ext::sh -c bad", "file:///tmp/repo"):
            lock = copy.deepcopy(self.lock)
            lock["components"][0]["url"] = remote
            with self.assertRaises(ValueError):
                sources.validate_lock(lock)

    def test_reject_moving_ref(self):
        self.lock["components"][0]["ref"] = "master"
        with self.assertRaises(ValueError):
            sources.validate_lock(self.lock)

    def test_reject_duplicate_component(self):
        self.lock["components"][1] = self.lock["components"][0]
        with self.assertRaises(ValueError):
            sources.validate_lock(self.lock)

    def test_destination_confinement(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(Path(tmp).resolve() / "linux", sources.destination_for(tmp, "linux"))
            with self.assertRaises(ValueError):
                sources.destination_for(tmp, "../linux")


class ComponentTests(unittest.TestCase):
    def run_component(self, content, role, expected_hash=None):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "part.fex"
            path.write_bytes(content)
            row = {"file": path.name, "role": role, "size": len(content),
                   "sha256": expected_hash or hashlib.sha256(content).hexdigest()}
            return components.scan(path, row)

    def test_old_runtime_rejected_but_transport_distinguished(self):
        data = b"U-Boot 2018.07-example\x00"
        runtime = self.run_component(data, "persistent_runtime_reference")
        self.assertIn("runtime U-Boot below 2025", runtime["version_problems"])
        transport = self.run_component(data, "flash_protocol_loader_candidate")
        self.assertEqual([], transport["version_problems"])
        self.assertFalse(transport["mainline_provenance_proven"])

    def test_old_kernel_rejected(self):
        result = self.run_component(b"ANDROID!\x00Linux version 6.6.98-test\x00", "persistent_kernel_reference")
        self.assertIn("runtime Linux below 6.18", result["version_problems"])

    def test_version_banner_is_not_provenance(self):
        result = self.run_component(b"Linux version 6.18.63-t736\x00", "persistent_kernel_reference")
        self.assertEqual([], result["version_problems"])
        self.assertFalse(result["mainline_provenance_proven"])

    def test_tampered_binary(self):
        result = self.run_component(b"changed", "flash_dram_loader_candidate", "0" * 64)
        self.assertFalse(result["hash_matches"])

    def test_boundary_spanning_banner(self):
        result = self.run_component(b"\x00" * (1024 * 1024 - 4) + b"U-Boot 2018.07-test\x00", "persistent_runtime_reference")
        self.assertIn("2018.07-test", result["uboot_banners"])

    def test_path_traversal_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                components.inventory(tmp, {"components": [{"file": "../boot.fex"}]})


if __name__ == "__main__":
    unittest.main()
