#!/usr/bin/env python3
"""Read-only source-v11 input contract and persistent U-Boot boot command.

The template is deliberately pending. Only explicit hardware acceptance and
exact frozen artifacts can produce a build plan; this never invents acceptance.
"""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import re
import subprocess
import sys

BASE_SHA = '935a58e933ec831c470a5ef20c89dd5f0f675f417d4fa43bbe0a831e91414469'
BASE_BYTES = 7802716160
ROOT_PARTUUID = 'a0085546-4166-744a-a353-fca9272b8e48'
RECOVERY_DTB_SHA = '6124591aab1cfc02b287a88872835b20a6d2dac70b3e7cd2b93bb9af36d3e82b'
BASE_ARGS = ('earlycon=uart8250,mmio32,0x02500000 console=ttyS0,115200 '
             'root=PARTUUID=' + ROOT_PARTUUID + ' rootwait rw clk_ignore_unused')
MODES = {
    'hs400-200': ('6.18.63-t736-storage-v24', 10, 3,
                 't736_mmc_pio.enable_hs200=1 t736_mmc_pio.hs200_max_hz=200000000 '
                 't736_mmc_pio.enable_hs400=1 t736_mmc_pio.hs400_max_hz=200000000'),
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def project_file(root, name):
    require(isinstance(name, str) and name, 'An explicit project file is required')
    relative = Path(name)
    require(not relative.is_absolute() and '..' not in relative.parts, 'Expected project-relative input')
    path = (root / relative).resolve(strict=True)
    require(path.is_relative_to(root) and path.is_file(), 'Input escapes project or is not a file')
    return path


def bound_file(root, item):
    require(isinstance(item, dict) and re.fullmatch(r'[a-f0-9]{64}', item.get('sha256', '')),
            'Input needs an explicit SHA256')
    path = project_file(root, item.get('path'))
    require(sha(path) == item['sha256'], 'Input hash mismatch: ' + item['path'])
    return path


def bootcommand(mode):
    require(mode in MODES, 'Unsupported accepted storage mode')
    fast_args = BASE_ARGS + ' ' + MODES[mode][3]
    # ENV_IS_NOWHERE does not provide persistence for a Linux-side env edit.
    # The fast defaults live in compiled CONFIG_BOOTCOMMAND. Recovery requires
    # an explicit serial U-Boot selection; booti kernel hangs cannot fall back.
    return (
        'setenv kernel_comp_addr_r 0x68000000; '
        'if test "${t736_recovery}" = "1"; then '
        'setenv t736_kernel /boot/t736-source-v16-recovery/vmlinuz-6.18.63-t736-storage-v16; '
        'setenv t736_initrd /boot/t736-source-v16-recovery/initrd.img-6.18.63-t736-storage-v16; '
        'setenv t736_dtb /boot/t736-source-v16-recovery/t736-mainline-bringup.dtb; '
        'setenv bootargs ' + BASE_ARGS + '; '
        'else setenv t736_kernel /boot/vmlinuz-6.18.63-t736-armbian; '
        'setenv t736_initrd /boot/initrd.img-6.18.63-t736-armbian; '
        'setenv t736_dtb /boot/dtb/allwinner/t736-mainline-bringup.dtb; '
        'setenv bootargs ' + fast_args + '; fi; '
        'if mmc dev 0 && ext4load mmc 0:4 0x60000000 ${t736_kernel} && '
        'setenv kernel_comp_size ${filesize} && '
        'ext4load mmc 0:4 0x64000000 ${t736_initrd}; then '
        'setenv rdsize ${filesize}; '
        'if ext4load mmc 0:4 0x63000000 ${t736_dtb}; then '
        'booti 0x60000000 0x64000000:${rdsize} 0x63000000; fi; fi'
    )


def storage_checks(record, release, kernel_sha, pvr_sha):
    """Consume the existing parent-authored bounded HS400 stage record."""
    require(record.get('schema') == 1 and record.get('kind') == 't736-storage-v24-hs400-stage-record'
            and record.get('kernel_release') == release, 'Wrong HS400 stage record')
    artifacts = record.get('fixed_artifacts', {})
    require(artifacts.get('package', {}).get('sha256') == kernel_sha
            and artifacts.get('pvr_sha256') == pvr_sha, 'Stage artifacts differ from image inputs')
    stage = record.get('stages', {}).get('hs400_200', {})
    require(stage.get('accepted') is True, 'HS400200 stage has not passed')
    ios = stage.get('software_ios', {})
    require(ios.get('actual_hz') == 200000000 and ios.get('timing') == 10
            and ios.get('width_bits') == 8 and ios.get('signal_voltage') == '1.80V'
            and ios.get('card_hs_timing') == '0x03' and ios.get('card_bus_width') == '0x06',
            'Actual HS400200/8bit/1.8V/EXT03-06 evidence is missing')
    tuning = stage.get('tuning', {})
    require(tuning.get('frequency_hz') == 200000000 and tuning.get('final_ext_csd_mode') == '03/06',
            'Tuning mode differs')
    for key in ('command_eye', 'data_strobe_eye'):
        eye = tuning.get(key, {})
        first, count, tap = [eye.get(name, -1) for name in
                             ('linear_window_first', 'linear_window_count', 'chosen_tap')]
        require(first >= 0 and count >= 12 and first + count <= 64
                and tap - first >= 5 and first + count - 1 - tap >= 5,
                'Insufficient linear eye/margins: ' + key)
        require(eye.get('successful_trials_per_passing_tap') == 20
                and eye.get('center_verification_transfers') == 16
                and int(eye.get('passing_bitmap', '0'), 16) & (1 << tap), 'Eye verification incomplete: ' + key)
    require(tuning['command_eye'].get('command_retries') == 0
            and tuning['command_eye'].get('opcode') == 13
            and tuning['data_strobe_eye'].get('opcode') == 8
            and tuning['data_strobe_eye'].get('bytes_per_read') == 512, 'Wrong CMD/DS verification protocol')
    readings = stage.get('readbacks', {})
    require(readings.get('all_three_passes_match') is True and readings.get('passes') == 3
            and readings.get('registers_per_pass') == 25, 'Stable controller readbacks missing')
    values = readings.get('values', {})
    require(values.get('SMHC3_CLK') == '0x81000001' and values.get('SMHC3_EDSD') == '0x80000000'
            and values.get('SMHC3_CSDC') == '0x00000006', 'Controller is not in expected HS400200 mode')
    integrity = stage.get('integrity', {})
    require(integrity.get('status') == 'PASS' and integrity.get('file_bytes', 0) >= 33554432
            and integrity.get('phases') == 2 and integrity.get('raw_device_access') is False
            and integrity.get('buffered_fallback') is False, 'Bounded direct-I/O integrity stage incomplete')
    for phase in integrity.get('phases_summary', []):
        require(phase.get('fsync_completed') is True and phase.get('reads')
                and all(item.get('hash_match') is True for item in phase['reads'])
                and any(item.get('covers_full_file') is True for item in phase['reads']), 'Integrity phase failed')
    require(len(integrity.get('phases_summary', [])) == 2, 'Both integrity phase summaries required')
    health = stage.get('post_probe_health', {})
    require(health.get('gpu_probe') == 'PASS' and health.get('frames', 0) >= 60
            and health.get('pvr_driver') == 'OK' and health.get('pvr_firmware') == 'OK'
            and health.get('pvr_reported_error_counters') == 0, 'GPU health stage incomplete')
    require(record.get('evidence'), 'Stage requires bound raw evidence')


def load_plan(root, manifest):
    root = Path(root).resolve(strict=True)
    source = Path(manifest).resolve(strict=True)
    require(source.is_relative_to(root), 'Input manifest must be in the project')
    data = json.loads(source.read_text())
    require(data.get('schema') == 1 and data.get('kind') == 't736-source-v11-inputs'
            and data.get('status') == 'accepted-inputs-ready', 'V11 input template is not accepted/ready')
    mode = data.get('storage_mode')
    require(mode in MODES, 'Select an explicitly accepted 200MHz storage mode')
    release, _, _, parameters = MODES[mode]
    base = root / 'output/rootfs-armbian-source-v10.ext4'
    require(base.is_file() and base.stat().st_size == BASE_BYTES and sha(base) == BASE_SHA,
            'Frozen source-v10 rootfs changed')
    require((root / 'build/source-rootfs-v10-state/finalize.complete').is_file(), 'V10 is not finalized')
    kernel = bound_file(root, data['kernel'])
    pvr = bound_file(root, data['pvr'])
    version = release.rsplit('-v', 1)[1]
    sys.path.insert(0, str(root / 'tools'))
    updater = importlib.import_module('validation.update_t736_storage_v' + version)
    updater.inputs(kernel, pvr)  # Exact archive and ABI checks, never device_check/apply.
    storage = bound_file(root, data['storage_acceptance'])
    acceptance = json.loads(storage.read_text())
    storage_checks(acceptance, release, sha(kernel), sha(pvr))
    for item in acceptance['evidence']:
        bound_file(root, item)
    gpu = bound_file(root, data['gpu_acceptance'])
    gpu_record = json.loads(gpu.read_text())
    require(gpu_record.get('schema') == 1 and gpu_record.get('status') == 'passed'
            and gpu_record.get('kernel_release') == release
            and gpu_record.get('kernel_deb_sha256') == sha(kernel), 'GPU acceptance does not bind kernel')
    require(all(gpu_record.get('checks', {}).get(key) is True for key in
                ('gbm_probe', 'x11_probe', 'gnome_hardware_renderer', 'gtk_hardware_renderer', 'hdmi_1080p')),
            'Full GPU profile acceptance is still required')
    require(gpu_record.get('evidence_logs'), 'GPU acceptance needs raw evidence')
    for name, digest in gpu_record['evidence_logs'].items():
        bound_file(root, {'path': name if '/' in name else 'evidence/' + name, 'sha256': digest})
    gst = bound_file(root, data['gstreamer_gl_deb'])
    fields = subprocess.check_output(['dpkg-deb', '-f', str(gst), 'Package', 'Version', 'Architecture'], text=True)
    require(fields.splitlines() == ['Package: gstreamer1.0-gl', 'Version: 1.26.2-1+deb13u2', 'Architecture: arm64'],
            'GstGL package differs from the board-validated version')
    recovery_dtb = root / 'output/independent-candidates/t736-cpu-topology-v16.dtb'
    require(sha(recovery_dtb) == RECOVERY_DTB_SHA, 'V16 recovery needs its accepted topology DTB')
    from validation.spl_pack_guard import load_spl
    from validation.cpu_d_pack_guard import load_cpu_d_package
    spl, provenance = load_spl(root, 'v4')
    load_cpu_d_package(root, spl, provenance)
    return {'schema': 1, 'variant': 'v11', 'status': 'INPUT_CHECKS_PASS_NOT_IMAGE_ACCEPTANCE',
            'manifest': str(source), 'manifest_sha256': sha(source), 'storage_mode': mode,
            'kernel_release': release, 'package_version': '6.18.63-' + version,
            'kernel': str(kernel), 'pvr': str(pvr), 'gpu_acceptance': str(gpu),
            'storage_acceptance': str(storage), 'gstreamer_gl_deb': str(gst),
            'base_rootfs': str(base), 'base_sha256': BASE_SHA, 'base_bytes': BASE_BYTES,
            'candidate_build': str(root / ('build/linux-storage-v' + version)),
            'recovery_dtb': str(recovery_dtb), 'boot_parameters': parameters,
            'bootcommand': bootcommand(mode), 'recovery_selection': 'setenv t736_recovery 1; run bootcmd',
            'automatic_kernel_hang_fallback': False, 'software_fel_implementation_changed': False,
            'mutter_experiment_included': False, 'final_image_cold_boot_validated': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(load_plan(args.root, args.manifest), indent=2))


if __name__ == '__main__':
    main()
