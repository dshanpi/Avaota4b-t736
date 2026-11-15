#!/usr/bin/env python3
"""Build/test source-only eMMC PIO core; does not make a deployable SPL."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
if root != Path('/home/ubuntu/t736-mainline'):
    raise SystemExit('Run on the isolated Linux build project')
source = root / 'spl/storage'
out = root / 'build/spl-emmc'
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wshadow', '-Wconversion', '-Os']
commands = [
    ['cc', *flags, '-fsanitize=address,undefined', str(source/'emmc.c'),
     str(source/'test_emmc.c'), '-o', str(out/'test_emmc')],
    [str(out/'test_emmc')],
    ['arm-linux-gnueabi-gcc', *flags, '-mcpu=cortex-a55', '-marm',
     '-mfloat-abi=soft', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
     '-fstack-usage', '-c', str(source/'emmc.c'), '-o', str(out/'emmc.o')],
]
for command in commands:
    print(json.dumps(command), flush=True)
    subprocess.run(command, check=True)
undefined = subprocess.check_output(['arm-linux-gnueabi-nm', '-u', str(out/'emmc.o')], text=True)
if undefined.strip():
    raise SystemExit('Freestanding dependency: ' + undefined)
files = [source/'emmc.c', source/'emmc.h', source/'test_emmc.c', Path(__file__), out/'emmc.o']
result = dict(native_sanitizer_tests_pass=True, arm32_compile_pass=True,
              undefined_symbols=undefined,
              stack_usage=(out/'emmc.su').read_text(),
              inputs_and_object_sha256={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
              board_power_pad_pinmux_prerequisites_implemented=False,
              hardware_tested=False, independent_boot_tested=False,
              deployable=False, commands=commands)
(root/'evidence/spl-emmc-check.json').write_text(json.dumps(result, indent=2)+'\n')
