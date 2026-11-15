#!/usr/bin/env python3
"""Build/test source timing components; never deploy or touch board MMIO."""
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run in isolated Linux build project /home/ubuntu/t736-mainline')
    source = root / 'spl/lpddr4'
    build = root / 'build/lpddr4-timing'
    build.mkdir(parents=True, exist_ok=True)
    files = [source / n for n in ('timing.c', 'timing.h', 'test_timing.c',
                                  'controller.c', 'controller.h')]
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion',
             '-Wshadow', '-O2', '-I', str(source), '-I', str(build)]
    observation = root / 'evidence/lpddr4-timing-board-observation.log'
    observed = {}
    # This restricted whitelist corresponds exactly to the candidate subset.
    offsets = [0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c,
               0x024, 0x030, 0x034, 0x038, 0x05c, 0x060, 0x064,
               0x580, 0x584, 0x588, 0x600, 0x604, 0x608]
    if observation.exists():
        data = observation.read_text()
        if 'INSMOD_RC=0' not in data or 'RMMOD_RC=0' not in data:
            raise SystemExit('Timing observation is incomplete')
        for addr, value in re.findall(r'@0x([0-9a-f]+) = ([0-9a-f]{8})', data):
            addr, value = int(addr, 16), int(value, 16)
            for base in (0x0a100000, 0x0a500000):
                if addr - base in offsets:
                    if addr in observed and observed[addr] != value:
                        raise SystemExit('Conflicting timing observation')
                    observed[addr] = value
        if len(observed) != len(offsets) * 2:
            raise SystemExit(f'Expected 42 observed timing words, got {len(observed)}')
        text = '#include <stdint.h>\nstatic const struct { uint32_t offset, value; } timing_observed[] = {\n'
        for addr, value in sorted(observed.items()):
            offset = addr - (0x0a100000 if addr < 0x0a500000 else 0x0a500000)
            text += f'    {{0x{offset:03x}u, 0x{value:08x}u}},\n'
        (build / 'timing_board_observed.h').write_text(text + '};\n')
        flags.append('-DT736_TIMING_BOARD_OBSERVATION=1')
        files.append(observation)
    commands = [
        ['cc', *flags, '-fsanitize=address,undefined',
         str(source / 'controller.c'), str(source / 'timing.c'),
         str(source / 'test_timing.c'), '-o', str(build / 'test_timing')],
        [str(build / 'test_timing')],
        ['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55', '-marm',
         '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin',
         '-fno-stack-protector', '-c', str(source / 'timing.c'),
         '-o', str(build / 'timing.o')],
    ]
    for command in commands:
        print(json.dumps(command), flush=True)
        subprocess.run(command, check=True)
    undefined = subprocess.check_output(['arm-linux-gnueabi-nm', '-u',
                                         str(build / 'timing.o')], text=True)
    allowed = {'__aeabi_uldivmod', 't736_lp4_set_update'}
    symbols = set(re.findall(r'\bU (\S+)', undefined))
    if symbols - allowed:
        raise SystemExit(f'Unexpected external dependency: {symbols - allowed}')
    files += [Path(__file__), build / 'timing.o']
    result = {
        'commands': commands,
        'sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in files},
        'native_sanitized_tests_pass': True,
        'arm32_compile_pass': True,
        'board_observed_words_compared': len(observed),
        'undefined_symbols': sorted(symbols),
        'vendor_executable_data_linked': False,
        'hardware_registers_written': False,
        'phy_training_implemented': False,
        'complete_controller_initialization': False,
        'coldboot_tested': False,
        'deployable': False,
    }
    (root / 'evidence/lpddr4-timing-check.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
