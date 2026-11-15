#!/usr/bin/env python3
"""Native fault tests and freestanding compile; does not access target MMIO."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def validate_ccu_hardware(root):
    """Accept this one reviewed SRAM run only with its source/artifact chain."""
    path = root / 'evidence/t736-ccu-cold-sram-hardware-20261114.json'
    if not path.exists():
        return None, []
    data = json.loads(path.read_text())
    words = struct.unpack('<32I', bytes.fromhex(data['report_hex']))
    before = (0x41304700, 0, 1, 0xc3000000, 0x10001, 0x84000000)
    after = (0xf9004a00, 0x80000003, 0x10001, *before[3:])
    probe = data['probe']
    if not data.get('success') or not probe.get('passed') or \
            data['cpu_preflight']['version']['raw_chip_id'] != '0x190300' or \
            words[:3] != (0x54374343, 1, 2) or words[3] & 0x3005 or \
            words[4] != 24000000 or words[5] < 240 or words[6:8] != (6, 0) or \
            words[8:14] != before or words[14:20] != after or \
            words[20:26] != (7, 2, 4, 0, 63, 63) or \
            words[26] not in (0, 24000000) or not words[27] & 1 or \
            words[28] != 24000000 or any(words[29:]) or \
            any(data.get(key) is not False for key in
                ('storage_accessed', 'pmic_accessed', 'ddr_initialized')) or \
            any(probe.get(key) is not False for key in
                ('storage_accessed', 'pmic_accessed', 'ddr_initialized', 'phy_accessed')):
        raise SystemExit('Invalid CCU hardware report')
    manifest_path = root / 'build/ccu-probe/build-record.json'
    manifest = json.loads(manifest_path.read_text())
    files = [path, manifest_path]
    for name, digest in manifest['input_sha256'].items():
        candidate = root / name
        if hashlib.sha256(candidate.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'CCU hardware source changed since reviewed build: {name}')
        files.append(candidate)
    image = root / 'build/ccu-probe/t736-ccu-probe.bin'
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    if digest != data['upload_readback_sha256'] or \
            digest != manifest['output_sha256']['t736-ccu-probe.bin']:
        raise SystemExit('CCU hardware payload hash mismatch')
    files.append(image)
    return {'log': str(path.relative_to(root)), 'payload_sha256': digest,
            'completed_stage': words[6], 'result': words[7],
            'writes': list(words[20:23]), 'ten_us_delay_ticks': words[5],
            'fabric_preserved': True, 'early_boot0_fel_context': True}, files


def main():
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run on the isolated Linux build host project')
    build = root / 'build/lpddr4-phy-host'
    build.mkdir(parents=True, exist_ok=True)
    evidence = root / 'evidence'
    evidence.mkdir(exist_ok=True)
    log_path = evidence / 'lpddr4-phy-host-check.log'
    log_lines = []
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion',
             '-Wshadow', '-O2']
    ccu_observations = [evidence / f't736-ccu-{group}-sram-hardware-20261114.json'
                        for group in ('ddr', 'fabric')]
    board_state = {}
    if all(p.exists() for p in ccu_observations):
        for p in ccu_observations:
            data = json.loads(p.read_text())
            if not data.get('success') or data.get('mmio_writes') is not False or \
                    data['version']['raw_chip_id'] != '0x190300' or \
                    not data['probe']['guard_passed']:
                raise SystemExit(f'Invalid read-only CCU observation: {p}')
            for r in data['probe']['registers']:
                addr, value = int(r['address'], 16), int(r['value'], 16)
                if addr in board_state:
                    raise SystemExit('Duplicate CCU observation address')
                board_state[addr] = value
        expected = {0x02002020, 0x02002c00, 0x02002c0c,
                    0x02002580, 0x02002584, 0x02002588}
        if set(board_state) != expected:
            raise SystemExit('Read-only CCU observation address set mismatch')
        header = '#include <stdint.h>\nstatic const struct { uint32_t address, value; } ccu_board_observed[] = {\n'
        for addr, value in sorted(board_state.items()):
            header += f'    {{0x{addr:08x}u, 0x{value:08x}u}},\n'
        (build / 'ccu_board_observed.h').write_text(header + '};\n')
        flags += ['-I', str(build), '-DT736_CCU_BOARD_OBSERVATION=1']
    commands = [
        ['cc', *flags, '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         str(source / 'host.c'), str(source / 'clocks.c'), str(source / 'test_host.c'),
         '-o', str(build / 'test_host')],
        [str(build / 'test_host')],
        ['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55', '-marm',
         '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-c', str(source / 'host.c'),
         '-o', str(build / 'host.o')],
        ['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55', '-marm',
         '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-c', str(source / 'clocks.c'),
         '-o', str(build / 'clocks.o')],
        ['arm-linux-gnueabi-ld', '-r', str(build / 'host.o'),
         str(build / 'clocks.o'), '-o', str(build / 'phy-host-components.o')],
    ]
    for command in commands:
        line = json.dumps(command)
        print(line, flush=True)
        log_lines.append(line + '\n')
        proc = subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        print(proc.stdout, end='', flush=True)
        log_lines.append(proc.stdout)
        log_path.write_text(''.join(log_lines))
        proc.check_returncode()
    undefined = subprocess.check_output(['arm-linux-gnueabi-nm', '-u',
                                         str(build / 'phy-host-components.o')], text=True)
    symbols = set(re.findall(r'\bU (\S+)', undefined))
    if symbols - {'__aeabi_uidivmod', '__aeabi_uidiv'}:
        raise SystemExit(f'Unexpected dependencies: {symbols}')
    files = [source / n for n in ('host.c', 'host.h', 'clocks.c', 'clocks.h', 'test_host.c', 'check.py',
                                  'provenance.json', 'README.md')]
    files += [build / 'host.o', build / 'clocks.o', build / 'phy-host-components.o', source.parent / 'controller.h',
              source.parent / 'timing.h']
    if board_state:
        files += ccu_observations
    ccu_hardware, hardware_files = validate_ccu_hardware(root)
    files += hardware_files
    observation = root / 'evidence/lpddr4-phy-host-readonly-20261114.log'
    real_owner_refusal = False
    if observation.exists():
        log = observation.read_text()
        if re.search(r'MuxSel @0x0aaa0000 = 0001', log) and \
                'INSMOD_RC=0' in log and 'RMMOD_RC=0' in log:
            real_owner_refusal = True
            files.append(observation)
    result = {
        'commands': commands,
        'sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in files},
        'native_asan_ubsan_pass': True,
        'arm32_freestanding_compile_pass': True,
        'undefined_symbols': sorted(symbols),
        'board_running_phy_owner_refusal_observed': real_owner_refusal,
        'board_ccu_writes_tested': ccu_hardware is not None,
        'board_phy_writes_tested': False,
        'ccu_hardware': ccu_hardware,
        'vendor_executable_data_linked': False,
        'ccu_initial_clock_component_implemented': True,
        'nsi_mbus_configuration_preserved': True,
        'nsi_mbus_actual_frequency_measured': False,
        'board_fel_register_state_simulated': {hex(k): hex(v) for k, v in board_state.items()},
        'initial_dram0_mhz': 450,
        'prcm_smc_clock_reset_setup_implemented': False,
        'complete_cold_clock_tree': False,
        'phy_electrical_configuration_complete': False,
        'training_implemented': False,
        'coldboot_tested': False,
        'deployable': False,
    }
    (evidence / 'lpddr4-phy-host-check.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
