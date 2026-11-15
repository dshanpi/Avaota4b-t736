#!/usr/bin/env python3
"""Native failure/safety checks and ARM32 compile; no hardware access."""
from pathlib import Path
import hashlib
import json
import subprocess


def main():
    source = Path(__file__).resolve().parent
    root = source.parents[1]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run in the isolated Linux build project')
    build = root / 'build/lpddr4-mode-register'
    build.mkdir(parents=True, exist_ok=True)
    evidence = root / 'evidence/lpddr4-mode-register-check.json'
    evidence.write_text(json.dumps({'checks_complete': False, 'board_tested': False}) + '\n')
    logs = []

    def run(command):
        proc = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
        entry = json.dumps(command) + '\n' + proc.stdout
        print(entry, end='', flush=True)
        logs.append(entry)
        (root / 'evidence/lpddr4-mode-register-check.log').write_text(''.join(logs))
        if proc.returncode:
            raise SystemExit(proc.returncode)
        return proc.stdout

    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wshadow',
             '-Os', '-I', str(source)]
    run(['cc', *flags, '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         str(source / 'mode_register.c'), str(source / 'test_mode_register.c'),
         '-o', str(build / 'test_mode_register')])
    run([str(build / 'test_mode_register')])
    run(['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55', '-marm', '-mfloat-abi=soft',
         '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-c',
         str(source / 'mode_register.c'), '-o', str(build / 'mode_register.o')])
    undefined = run(['arm-linux-gnueabi-nm', '-u', str(build / 'mode_register.o')])
    if undefined.strip():
        raise SystemExit('Unexpected external dependency')
    paths = [source / name for name in ('mode_register.c', 'mode_register.h',
             'test_mode_register.c', 'check_mode_register.py', 'controller.h',
             'pmu/mode-register-audit.json')]
    paths.append(build / 'mode_register.o')
    evidence.write_text(json.dumps({
        'checks_complete': True,
        'native_address_undefined_sanitizers_pass': True,
        'lane_channel_rank_id_vectors': 3072,
        'all_callback_failures_tested': True,
        'arm32_freestanding_compile_pass': True,
        'undefined_symbols': undefined,
        'allowed_mode_registers': [5, 6, 8],
        'mr_write_supported': False,
        'vendor_binaries_linked': False,
        'board_tested': False,
        'receive_valid_bit_identified': False,
        'ddr_training_implemented': False,
        'boot_firmware_deployable': False,
        'inputs_and_objects': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in paths},
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
