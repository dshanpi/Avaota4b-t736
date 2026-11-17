#!/usr/bin/env python3
"""Safety boundaries for the draft's persistent boot selection and inputs."""
import json
import copy
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from validation.source_v11_plan import bootcommand, load_plan, project_file, storage_checks, BASE_ARGS


class SourceV11DraftTest(unittest.TestCase):
    def test_persistent_hs400_and_explicit_recovery(self):
        command = bootcommand('hs400-200')
        recovery, default = command.split('; else ', 1)
        self.assertIn('test "${t736_recovery}" = "1"', recovery)
        self.assertIn('/boot/t736-source-v16-recovery/', recovery)
        self.assertNotIn('t736_mmc_pio.', recovery)
        self.assertIn('setenv bootargs ' + BASE_ARGS, recovery)
        for option in ('enable_hs200=1', 'hs200_max_hz=200000000',
                       'enable_hs400=1', 'hs400_max_hz=200000000'):
            self.assertEqual(default.count('t736_mmc_pio.' + option), 1)
        self.assertEqual(command.count('booti '), 1)
        for unexpected in ('armbianEnv', 'saveenv', 'mmc write', 'mmc erase', 'reset;'):
            self.assertNotIn(unexpected, command)

    def test_unsupported_or_unaccepted_rate(self):
        with self.assertRaises(RuntimeError):
            bootcommand('hs400-100')
        with self.assertRaises(RuntimeError):
            bootcommand('latest')

    def test_v11_cannot_silently_select_hs200(self):
        with self.assertRaises(RuntimeError):
            bootcommand('hs200-200')

    def test_real_stage_record_and_negative_mode_window_integrity(self):
        record = json.loads((ROOT / 'evidence/storage-v24-hs400-stage-record-20261116.json').read_text())
        artifacts = record['fixed_artifacts']
        args = (record['kernel_release'], artifacts['package']['sha256'], artifacts['pvr_sha256'])
        storage_checks(record, *args)
        for mutation in ('mode', 'window', 'integrity', 'gpu', 'package'):
            changed = copy.deepcopy(record)
            stage = changed['stages']['hs400_200']
            if mutation == 'mode': stage['software_ios']['actual_hz'] = 100000000
            if mutation == 'window': stage['tuning']['data_strobe_eye']['linear_window_count'] = 11
            if mutation == 'integrity': stage['integrity']['phases_summary'][0]['reads'][0]['hash_match'] = False
            if mutation == 'gpu': stage['post_probe_health']['gpu_probe'] = 'FAIL'
            if mutation == 'package': changed['fixed_artifacts']['package']['sha256'] = '0'*64
            with self.assertRaises(RuntimeError):
                storage_checks(changed, *args)

    def test_pending_template_fails_before_any_artifact_access(self):
        template = json.loads((ROOT / 'config/source-v11-inputs.template.json').read_text())
        self.assertEqual(template['storage_mode'], 'hs400-200')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / 'pending.json'
            path.write_text(json.dumps(template))
            with self.assertRaisesRegex(RuntimeError, 'not accepted/ready'):
                load_plan(root, path)
            self.assertEqual(list(root.iterdir()), [path])

    def test_project_input_cannot_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(RuntimeError):
                project_file(root, '../outside')
            with self.assertRaises(RuntimeError):
                project_file(root, str(root / 'absolute'))


if __name__ == '__main__':
    unittest.main()
