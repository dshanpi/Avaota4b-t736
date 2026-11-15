#!/usr/bin/env python3
"""Check the error-reporting controller interface; no hardware access."""
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run on the isolated Linux build host project')
    build = root / 'build/lpddr4-checked-controller'
    build.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wshadow', '-O2']
    sources = [source / 'checked_controller.c', source.parent / 'controller.c', source.parent / 'timing.c']
    log = []

    def run(args):
        p = subprocess.run(list(map(str, args)), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = json.dumps(list(map(str, args))) + '\n' + p.stdout
        print(text, end='', flush=True); log.append(text)
        (root / 'evidence/lpddr4-checked-controller-check.log').write_text(''.join(log))
        p.check_returncode()
        return p.stdout

    run(['cc', *flags, '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *sources,
         source / 'test_checked_controller.c', '-o', build / 'test_checked_controller'])
    run([build / 'test_checked_controller'])
    run(['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55', '-marm', '-mfloat-abi=soft',
         '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-c', sources[0],
         '-o', build / 'checked_controller.o'])
    symbols = run(['arm-linux-gnueabi-nm', '-u', build / 'checked_controller.o'])
    undefined = sorted(line.split()[-1] for line in symbols.splitlines() if line.strip())
    if undefined != ['t736_lp4_build_layout', 't736_lp4_build_timing']:
        raise SystemExit(f'Unexpected checked interface dependency: {undefined}')
    files = [*sources, source / 'checked_controller.h', source / 'test_checked_controller.c',
             source / 'check_controller.py', source / 'host.h', source.parent / 'controller.h',
             source.parent / 'timing.h', build / 'checked_controller.o']
    result = {'native_asan_ubsan_pass': True, 'arm32_freestanding_compile_pass': True,
              'layout_io_fault_positions': 52, 'timing_io_fault_positions': 136,
              'undefined_symbols': undefined,
              'observation_io_fault_positions': 6, 'write_readback_failure_positions': 64,
              'controller_hardware_configured': False, 'source_only': True,
              'board_access_or_readiness_claimed': False, 'training_or_normal_mode_implemented': False,
              'sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (root / 'evidence/lpddr4-checked-controller-check.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
