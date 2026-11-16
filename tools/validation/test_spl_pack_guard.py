"""Synthetic offline rejection tests; these records are not hardware evidence."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import spl_pack_guard as guard


class SplGuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='t736-synthetic-spl-')
        self.root = Path(self.temp.name)
        self.build = self.root / 'build/emmc-spl-v4'
        self.build.mkdir(parents=True)
        self.source = self.root / 'spl/synthetic.c'
        self.source.parent.mkdir()
        self.source.write_bytes(b'/* Synthetic offline fixture only. */\n')
        data = bytearray(64)
        data[4:12] = b'eGON.BT0'
        struct.pack_into('<I', data, 16, len(data))
        checksum = self.checksum(data)
        self.packed = self.build / 'emmc-spl.bin'
        self.packed.write_bytes(data)
        self.record = {'kind': 'source-emmc-spl-with-authorized-ddr-library', 'variant': 'v4',
                       'entry': '0x47000', 'window_end': '0x7fc00', 'vendor_ddr_sha256': guard.DDR_SHA256,
                       'packed_bytes': len(data), 'packed_sha256': hashlib.sha256(data).hexdigest(),
                       'egon_checksum': hex(checksum), 'sources': {'spl/synthetic.c': guard.sha(self.source)}}
        self.fixture_pin = self.record['packed_sha256']
        self.write_record()

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def checksum(data):
        struct.pack_into('<I', data, 12, 0x5f0a6c39)
        checksum = sum(struct.unpack('<%dI' % (len(data) // 4), data)) & 0xffffffff
        struct.pack_into('<I', data, 12, checksum)
        return checksum

    def write_record(self):
        (self.build / 'build-record.json').write_text(json.dumps(self.record))

    def load(self):
        # Only this synthetic fixture changes the pin; production constants stay intact.
        with mock.patch.dict(guard.LEGACY_SPL, {'v4': self.fixture_pin}):
            return guard.load_spl(self.root, 'v4')

    def test_production_pins_unchanged(self):
        self.assertEqual(guard.LEGACY_SPL, {
            'v3': '7a3b30d3a0349e2b62d06fa07699cf1284af1e60a5704d4f1330aaf5011532db',
            'v4': 'a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea'})

    def test_defaults_select_historical_spl(self):
        self.assertEqual(guard.resolve_variant('v3'), 'v3')
        for image_variant in ('v4', 'v5', 'v6', 'v7', 'v8', 'v9', 'v10'):
            with self.subTest(image_variant=image_variant):
                self.assertEqual(guard.resolve_variant(image_variant), 'v4')
                self.assertEqual(guard.resolve_variant(image_variant, 'v4'), 'v4')
        with self.assertRaisesRegex(RuntimeError, 'keeps its original SPL'):
            guard.resolve_variant('v3', 'v4')

    def test_every_explicit_v5_selection_is_rejected(self):
        for image_variant in ('v3', 'v4', 'v5', 'v6', 'v7', 'v8', 'v9', 'v10', 'v11'):
            with self.subTest(image_variant=image_variant):
                with self.assertRaisesRegex(RuntimeError, 'SPL v5 is retired'):
                    guard.resolve_variant(image_variant, 'v5')

    def test_direct_v5_load_is_rejected_before_reading_files(self):
        with self.assertRaisesRegex(RuntimeError, 'SPL v5 is retired'):
            guard.load_spl(self.root / 'missing', 'v5')

    def test_retired_acceptance_api_cannot_authorize_v5(self):
        for acceptance in (None, self.root / 'missing', {'status': 'passed'}):
            with self.subTest(acceptance=acceptance):
                with self.assertRaisesRegex(RuntimeError, 'SPL v5 is retired'):
                    guard.validate_cpu_acceptance(self.root, acceptance, {}, {})

    def test_unknown_variants_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Unsupported source image revision'):
            guard.resolve_variant('v11')
        with self.assertRaisesRegex(RuntimeError, 'Unsupported SPL variant'):
            guard.resolve_variant('v10', 'v6')
        with self.assertRaisesRegex(RuntimeError, 'Unsupported SPL build'):
            guard.load_spl(self.root, 'v6')

    def test_pinned_historical_v4_does_not_claim_current_source_match(self):
        data, provenance = self.load()
        self.assertEqual(data, self.packed.read_bytes())
        self.assertFalse(provenance['current_sources_verified'])
        self.assertEqual(provenance['packed_sha256'], self.fixture_pin)
        self.source.write_bytes(b'changed shared source')
        _, changed = self.load()
        self.assertEqual(changed['sources'], self.record['sources'])
        self.assertFalse(changed['current_sources_verified'])

    def test_original_v3_record_can_omit_variant_only_when_v3_is_pinned(self):
        self.build.rename(self.root / 'build/emmc-spl-v3')
        self.build = self.root / 'build/emmc-spl-v3'
        self.record.pop('variant')
        self.write_record()
        with mock.patch.dict(guard.LEGACY_SPL, {'v3': self.fixture_pin}):
            _, provenance = guard.load_spl(self.root, 'v3')
        self.assertEqual(provenance['variant'], 'v3')
        self.assertFalse(provenance['current_sources_verified'])

    def test_v4_record_cannot_omit_variant(self):
        self.record.pop('variant')
        self.write_record()
        with self.assertRaisesRegex(RuntimeError, 'build identity mismatch'):
            self.load()

    def test_binary_drift_is_rejected(self):
        data = bytearray(self.packed.read_bytes())
        data[-1] ^= 1
        self.packed.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, 'SPL differs from build record'):
            self.load()

    def test_bad_checksum_is_rejected_even_with_updated_record_hash(self):
        data = bytearray(self.packed.read_bytes())
        data[-1] ^= 1
        self.packed.write_bytes(data)
        self.record['packed_sha256'] = guard.sha(self.packed)
        self.write_record()
        with self.assertRaisesRegex(RuntimeError, 'Invalid eGON checksum'):
            self.load()

    def test_repacked_binary_is_rejected_by_historical_pin(self):
        data = bytearray(self.packed.read_bytes())
        data[-1] ^= 1
        checksum = self.checksum(data)
        self.packed.write_bytes(data)
        self.record['packed_sha256'] = guard.sha(self.packed)
        self.record['egon_checksum'] = hex(checksum)
        self.write_record()
        with self.assertRaisesRegex(RuntimeError, 'Historical SPL differs'):
            self.load()

    def test_wrong_ddr_or_identity_is_rejected(self):
        original = self.record.copy()
        for key, value, error in (
                ('vendor_ddr_sha256', '0' * 64, 'DDR library provenance'),
                ('variant', 'v5', 'build identity mismatch'),
                ('kind', 'forged', 'build identity mismatch'),
                ('entry', '0x0', 'SRAM layout')):
            with self.subTest(key=key):
                self.record = dict(original, **{key: value})
                self.write_record()
                with self.assertRaisesRegex(RuntimeError, error):
                    self.load()

    def test_project_paths_cannot_escape(self):
        for relative in ('../outside', '/absolute', 'build/../../outside'):
            with self.subTest(relative=relative):
                with self.assertRaisesRegex(RuntimeError, 'project-relative'):
                    guard.project_file(self.root, relative)


if __name__ == '__main__':
    unittest.main()
