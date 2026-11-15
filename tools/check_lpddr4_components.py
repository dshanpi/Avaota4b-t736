#!/usr/bin/env python3
"""Test source-only LPDDR4 building blocks against real board observations.

Produces native test and freestanding ARM objects only, never an SPL or raw.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run in the isolated Linux build project')
    source = root / 'spl/lpddr4'
    build = root / 'build/lpddr4-components'
    build.mkdir(parents=True, exist_ok=True)
    log = root / 'evidence/lpddr4-addressmap-observation.log'
    data = log.read_text()
    if 'INSMOD_RC=0' not in data or 'RMMOD_RC=0' not in data:
        raise SystemExit('Missing completed board observation')
    words = {}
    for name, addr, value in re.findall(
            r't736-lp4-observe: (\w+) @0x([0-9a-f]+) = ([0-9a-f]{8})', data):
        if re.fullmatch(r'CH[01]_(?:MSTR|ADDR_\w+)', name):
            address, number = int(addr, 16), int(value, 16)
            if address in words and words[address] != number:
                raise SystemExit('Conflicting board observations')
            words[address] = number
    if len(words) != 22:
        raise SystemExit(f'Expected 22 observed layout words, got {len(words)}')
    header = '#include <stdint.h>\nstatic const struct { uint32_t address, value; } observed_layout[] = {\n'
    header += ''.join(f'    {{0x{a:08x}u, 0x{v:08x}u}},\n' for a, v in sorted(words.items()))
    (build / 'board_observed.h').write_text(header + '};\n')
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wconversion',
             '-Wshadow', '-O2', '-I', str(source), '-I', str(build)]
    cfiles = [source / 'controller.c', source / 'axp8191.c']
    commands = [['cc', *flags, '-fsanitize=address,undefined',
                 *map(str, cfiles), str(source / 'test_components.c'),
                 '-o', str(build / 'test_components')],
                [str(build / 'test_components')]]
    for cfile in cfiles:
        commands.append(['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55',
                         '-marm', '-mfloat-abi=soft', '-ffreestanding',
                         '-fno-builtin', '-fno-stack-protector', '-c', str(cfile),
                         '-o', str(build / (cfile.stem + '.o'))])
    for command in commands:
        print(json.dumps(command), flush=True)
        subprocess.run(command, check=True)
    undefined = subprocess.check_output(
        ['arm-linux-gnueabi-nm', '-u', *map(str, build.glob('*.o'))], text=True)
    if re.search(r'\bU\s+(?:init_DRAM|sunxi_dram_init|.*pmu.*|.*imem.*|.*pie.*)\b', undefined):
        raise SystemExit('Unexpected external DDR initializer/firmware dependency')
    files = [*source.glob('*.c'), *source.glob('*.h'), log,
             Path(__file__), *build.glob('*.o')]
    evidence = {
        'commands': commands,
        'inputs_and_objects': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in sorted(files)},
        'board_observed_layout_words': len(words),
        'native_sanitized_tests_pass': True,
        'arm32_compile_pass': True,
        'undefined_symbols': undefined,
        'vendor_library_linked': False,
        'board_registers_written': False,
        'pmic_voltage_change_hardware_tested': False,
        'lpddr4_training_implemented': False,
        'independent_spl_implemented': False,
        'coldboot_tested': False,
        'raw_image_built': False,
        'flashed': False,
        'deployable': False,
    }
    (root / 'evidence/lpddr4-components-check.json').write_text(
        json.dumps(evidence, indent=2) + '\n')
    print('PASS: source components only; complete DDR initialization remains unimplemented')


if __name__ == '__main__':
    main()
