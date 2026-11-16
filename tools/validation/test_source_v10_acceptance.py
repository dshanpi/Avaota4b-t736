"""Offline rejection cases for browser acceptance; no hardware claims."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validation.source_v10_acceptance import check_firefox, RELEASE


class FirefoxEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.record = {'sandbox_disabled': False, 'blocklist_overridden': False, 'tests': [{
            'mode': 'baseline', 'complete': True, 'exit_code': 0, 'profile_removed': True,
            'MOZ_X11_EGL': None, 'MOZ_DRM_DEVICE': None, 'preferences': {},
            'session': {'capabilities': {'moz:platformVersion': RELEASE, 'moz:headless': False,
                                        'browserVersion': 'SYNTHETIC TEST'}},
            'support': {'sections': {'graphics-features-tbody':
                'Compositing\tWebRender\nWebGL 1 Driver Renderer\tPowerVR TEST\nWebGL 2 Driver Renderer\tPowerVR TEST\n'}},
        }]}

    def test_complete_alone_does_not_accept_software_compositor(self):
        self.record['tests'][0]['support']['sections']['graphics-features-tbody'] = 'Compositing\tWebRender (Software)\n'
        with self.assertRaisesRegex(RuntimeError, 'hardware WebRender'):
            check_firefox(self.record)

    def test_hardware_compositor_without_both_webgl_renderers_rejected(self):
        self.record['tests'][0]['support']['sections']['graphics-features-tbody'] = 'Compositing\tWebRender\n'
        with self.assertRaisesRegex(RuntimeError, 'WebGL hardware'):
            check_firefox(self.record)

    def test_forcing_cannot_be_called_baseline(self):
        for key in ('MOZ_X11_EGL', 'MOZ_DRM_DEVICE'):
            candidate = copy.deepcopy(self.record)
            candidate['tests'][0][key] = 'forced'
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                check_firefox(candidate)

    def test_other_kernel_or_disabled_sandbox_rejected(self):
        candidate = copy.deepcopy(self.record)
        candidate['tests'][0]['session']['capabilities']['moz:platformVersion'] = 'old-v15'
        with self.assertRaises(RuntimeError):
            check_firefox(candidate)
        self.record['sandbox_disabled'] = True
        with self.assertRaises(RuntimeError):
            check_firefox(self.record)

    def test_valid_component_result_never_claims_video_or_image_acceptance(self):
        result = check_firefox(self.record)
        self.assertFalse(result['hardware_video_decoding_accepted'])
        self.assertNotIn('final_image_cold_boot_validated', result)


if __name__ == '__main__':
    unittest.main()
