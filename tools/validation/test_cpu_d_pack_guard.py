"""Self-contained offline fixtures, never hardware acceptance evidence."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validation import cpu_d_pack_guard as guard


class CpuDBindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='synthetic-cpu-d-binding-')
        self.root = Path(self.temp.name)
        self.spl = bytes(guard.SPL_BYTES)
        self.spl_provenance = {'variant': 'v4', 'packed_sha256': guard.digest(self.spl),
                               'packed_bytes': guard.SPL_BYTES, 'vendor_ddr_sha256': guard.DDR_SHA256}
        words = [0] * 128
        words[:4] = [0x36333754, 0x314c5053, 1, 3]
        body = bytearray()
        self.images = []
        parsed = []
        for index, (lba, address, length, _) in enumerate(guard.IMAGES):
            payload = bytes([index + 1]) * length
            crc = zlib.crc32(payload)
            words[8 + index * 4:12 + index * 4] = [lba, address, length, crc]
            body.extend(payload + bytes((-length) % 512))
            self.write(guard.PAYLOAD_PATHS[index], payload)
            self.images.append((lba, address, length, guard.digest(payload)))
            parsed.append({'sector': lba, 'address': hex(address), 'bytes': length,
                           'crc32': f'{crc:08x}', 'sha256': guard.digest(payload),
                           'offset': (lba - guard.BASE_LBA) * 512})
        header = bytearray(struct.pack('<128I', *words))
        struct.pack_into('<I', header, 16, zlib.crc32(header))
        self.package = bytes(header + body)
        self.write(guard.PACKAGE_PATH, self.package)
        self.write(guard.CONFIG_PATH, b'SYNTHETIC_CONFIG\n')
        self.write('spl/synthetic.c', b'/* synthetic source */\n')
        self.build = {'variant': 'cpu-d', 'host_fault_tests_passed': True,
                      'a76_startup_hz': 416000000, 'two_stage': True,
                      'a76_target_hz': 1014000000, 'a55_target_hz': 1014000000,
                      'a55_d_buffered_trace': True,
                      'a55_d_n_readback_windows': ['write-N', 'following LOCK_EN'],
                      'dcdc3_dcdc4_required_selector_uv': 900000, 'cpu0_hotplug_fixed': False,
                      'payload_size': self.images[2][2], 'payload_sha256': self.images[2][3],
                      'config_sha256': guard.sha(self.root / guard.CONFIG_PATH),
                      'sources_sha256': {'spl/synthetic.c': guard.sha(self.root / 'spl/synthetic.c')}}
        self.write_json(guard.BUILD_PATH, self.build)
        self.manifest = {'variant': 'cpu-d-boot-package', 'sha256': guard.digest(self.package),
                         'bytes': len(self.package), 'sector_bytes': 512, 'start_lba': guard.BASE_LBA,
                         'sector_count': guard.SECTORS, 'end_lba_exclusive': guard.BASE_LBA + guard.SECTORS,
                         'required_end_below_lba': guard.LIMIT_LBA, 'images': parsed,
                         'required_existing_spl': {'variant': 'v4', 'sha256': guard.digest(self.spl), 'modified': False},
                         'build_record': {'path': guard.BUILD_PATH, 'sha256': guard.sha(self.root / guard.BUILD_PATH)}}
        self.write_json(guard.MANIFEST_PATH, self.manifest)
        # Patches only apply within load(); production pins are never rewritten.
        self.pins = {'SPL_V4_SHA': guard.digest(self.spl), 'PACKAGE_SHA': guard.digest(self.package),
                     'BUILD_SHA': guard.sha(self.root / guard.BUILD_PATH),
                     'MANIFEST_SHA': guard.sha(self.root / guard.MANIFEST_PATH), 'IMAGES': tuple(self.images)}

    def tearDown(self):
        self.temp.cleanup()

    def write(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def write_json(self, relative, value):
        self.write(relative, (json.dumps(value, indent=2) + '\n').encode())

    def load(self, **overrides):
        with mock.patch.multiple(guard, **dict(self.pins, **overrides)):
            return guard.load_cpu_d_package(self.root, self.spl, self.spl_provenance)

    def structural_check(self, data):
        # Repinning the outer synthetic hash exercises inner structural guards.
        with mock.patch.multiple(guard, **dict(self.pins, PACKAGE_SHA=guard.digest(data))):
            return guard.validate_package_bytes(data)

    @staticmethod
    def repair_header(data):
        struct.pack_into('<I', data, 16, 0)
        struct.pack_into('<I', data, 16, zlib.crc32(data[:512]))

    def test_production_pins_remain_fixed(self):
        self.assertEqual(guard.SPL_V4_SHA, 'a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea')
        self.assertEqual(guard.PACKAGE_SHA, 'f2ea2157acc3b65b2cc7416dfdba39650e7daa6679693c558fbbdcc30ef9780a')
        self.assertEqual(guard.BUILD_SHA, '46f5065b48f1b2a4e18c263d2d84d1a02077267137ca033848d03c2377863d2e')

    def test_binding_never_claims_hardware_acceptance(self):
        data, items, provenance = self.load()
        self.assertEqual(data, self.package)
        self.assertEqual(len(items), 3)
        self.assertTrue(provenance['artifact_binding_verified'])
        self.assertTrue(provenance['current_sources_verified'])
        self.assertEqual(provenance['validation_scope'], 'artifact-binding-only')
        self.assertFalse(provenance['hardware_evidence_checked'])
        self.assertFalse(provenance['final_image_cold_boot_validated'])
        self.assertFalse(provenance['recorded_elf_rechecked'])

    def test_wrong_spl_bytes_rejected_even_with_v4_label(self):
        self.spl = b'x' + self.spl[1:]
        with self.assertRaisesRegex(RuntimeError, 'fixed SPL v4 bytes'):
            self.load()

    def test_wrong_spl_provenance_rejected(self):
        for key, value in (('variant', 'v5'), ('packed_bytes', 64), ('vendor_ddr_sha256', '0' * 64)):
            with self.subTest(key=key):
                original = self.spl_provenance[key]
                self.spl_provenance[key] = value
                with self.assertRaisesRegex(RuntimeError, 'SPL provenance mismatch'):
                    self.load()
                self.spl_provenance[key] = original

    def test_altered_package_rejected(self):
        self.write(guard.PACKAGE_PATH, self.package[:-1] + b'x')
        with self.assertRaisesRegex(RuntimeError, 'package size/SHA mismatch'):
            self.load()

    def test_wrong_geometry_rejected_after_valid_header_crc(self):
        data = bytearray(self.package)
        struct.pack_into('<I', data, 8 * 4, 24578)
        self.repair_header(data)
        with self.assertRaisesRegex(RuntimeError, 'payload geometry mismatch'):
            self.structural_check(data)

    def test_wrong_payload_crc_rejected_after_valid_header_crc(self):
        data = bytearray(self.package)
        struct.pack_into('<I', data, 11 * 4, 0)
        self.repair_header(data)
        with self.assertRaisesRegex(RuntimeError, 'payload CRC mismatch'):
            self.structural_check(data)

    def test_nonzero_padding_rejected(self):
        data = bytearray(self.package)
        data[512 + self.images[0][2]] = 1
        with self.assertRaisesRegex(RuntimeError, 'nonzero sector padding'):
            self.structural_check(data)

    def test_stage64_payload_change_rejected(self):
        data = bytearray(self.package)
        data[512] ^= 1
        with self.assertRaisesRegex(RuntimeError, 'payload SHA mismatch'):
            self.structural_check(data)

    def test_build_source_set_change_rejected(self):
        self.build['sources_sha256'] = {'different.c': '0' * 64}
        self.write_json(guard.BUILD_PATH, self.build)
        with self.assertRaisesRegex(RuntimeError, 'build record changed'):
            self.load()

    def test_current_source_drift_rejected(self):
        self.write('spl/synthetic.c', b'changed shared source')
        with self.assertRaisesRegex(RuntimeError, 'CPU-D input changed'):
            self.load()

    def test_configuration_drift_rejected(self):
        self.write(guard.CONFIG_PATH, b'changed config')
        with self.assertRaisesRegex(RuntimeError, 'configuration changed'):
            self.load()

    def test_uboot_artifact_drift_rejected(self):
        self.write(guard.UBOOT_PATH, b'wrong U-Boot')
        with self.assertRaisesRegex(RuntimeError, 'U-Boot differs from fixed build'):
            self.load()

    def test_tfa_standalone_artifact_drift_rejected(self):
        self.write(guard.PAYLOAD_PATHS[1], b'wrong TF-A')
        with self.assertRaisesRegex(RuntimeError, 'embedded payload differs from source artifact'):
            self.load()

    def test_manifest_drift_rejected(self):
        self.manifest['hardware_tested'] = True
        self.write_json(guard.MANIFEST_PATH, self.manifest)
        with self.assertRaisesRegex(RuntimeError, 'manifest changed'):
            self.load()


if __name__ == '__main__':
    unittest.main()
