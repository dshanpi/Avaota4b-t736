#!/usr/bin/env python3
"""Check real LP4 wire commands and identify LP5 descriptor CA fields.

Uses pinned public HDL truth tables as the oracle, never vendor instruction
arrays or a second hand-written copy of the new encoder's expressions.
Only native test executables are emitted; nothing runs on a PHY.
"""
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess


def table(path):
    tree = ast.parse(path.read_text())
    for item in tree.body:
        if isinstance(item, ast.ClassDef) and item.name == 'Command':
            for node in item.body:
                if isinstance(node, ast.Assign) and any(
                        isinstance(target, ast.Name) and target.id == 'TRUTH_TABLE'
                        for target in node.targets):
                    return ast.literal_eval(node.value)
    raise ValueError('Missing public truth table')


def pins(description, fields):
    def expand(match):
        prefix, start, end = match.groups()
        return ' '.join(prefix + str(bit) for bit in range(int(start), int(end) + 1))
    description = re.sub(r'([A-Z]+)(\d+)-(\d+)', expand, description)
    result = 0
    for bit, token in enumerate(description.split()):
        if token in ('L', 'V', 'X'):
            value = 0
        elif token == 'H':
            value = 1
        else:
            match = re.fullmatch(r'([A-Z]+)(\d+)', token)
            if not match:
                raise ValueError('Unsupported field: ' + token)
            name, index = match.groups()
            value = (fields[name] >> int(index)) & 1
        result |= value << bit
    return result


def emit_array(name, vectors):
    width = len(vectors[0])
    return (f'static const unsigned char {name}[{len(vectors)}][{width}] = {{\n' +
            ''.join('{' + ','.join(map(str, vector)) + '},\n' for vector in vectors) + '};\n')


def main():
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    if root != Path('/home/ubuntu/t736-mainline'):
        raise SystemExit('Run in the isolated Linux build project')
    build = root / 'build/lpddr4-pmu-commands'
    build.mkdir(parents=True, exist_ok=True)
    report_file = root / 'evidence/lpddr4-pmu-commands-check.json'
    report_file.write_text(json.dumps({'checks_complete': False, 'deployable': False}) + '\n')
    lp4_path = source / 'reference/litedram-lpddr4-commands.py'
    lp5_path = source / 'reference/litedram-lpddr5-commands.py'
    manifest = json.loads((source / 'command-source-manifest.json').read_text())
    for item in manifest['files']:
        path = source / item['path']
        # apply_patch may add a final blank line; no statement content changes.
        data = path.read_bytes().replace(b'\r\n', b'\n').rstrip(b'\n') + b'\n'
        blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if blob != item['git_blob']:
            raise SystemExit('Public source reference changed: ' + str(path))
    lp4, lp5 = table(lp4_path), table(lp5_path)
    lp4_mrw, lp4_mrr, lp4_mpc, lp5_mrw, lp5_mrr, lp5_mpc = [], [], [], [], [], []
    for mr in range(64):
        for op in range(256):
            lp4_mrw.append([pins(edge, {'MA': mr, 'OP': op})
                            for cmd in ('MRW-1', 'MRW-2') for edge in lp4[cmd]])
        lp4_mrr.append([pins(edge, {'MA': mr, 'C': mr})
                        for cmd in ('MRR-1', 'CAS-2') for edge in lp4[cmd]])
    for op in range(128):
        lp4_mpc.append([pins(edge, {'OP': op}) for edge in lp4['MPC']])
    for mr in range(128):
        for op in range(256):
            lp5_mrw.append([pins(edge, {'MA': mr, 'OP': op})
                            for cmd in ('MRW-1', 'MRW-2') for edge in lp5[cmd].split('|')])
        lp5_mrr.append([pins(edge, {'MA': mr}) for edge in lp5['MRR'].split('|')])
    for op in range(256):
        lp5_mpc.append([pins(edge, {'OP': op}) for edge in lp5['MPC'].split('|')])
    vectors = build / 'command_vectors.h'
    vectors.write_text('/* Generated test data from BSD-2-Clause LiteDRAM truth tables. */\n' +
        ''.join(emit_array(name, data) for name, data in (
            ('lp4_mrw', lp4_mrw), ('lp4_mrr', lp4_mrr), ('lp4_mpc', lp4_mpc),
            ('lp5_mrw', lp5_mrw), ('lp5_mrr', lp5_mrr), ('lp5_mpc', lp5_mpc))))
    # Compile exactly the pinned public encoder expressions, not a paraphrase.
    inferno = root / 'sources/audit/a733-pmu-source-2f5e45fa'
    train = inferno / 'pmu_train.c'
    raw = train.read_bytes().replace(b'\r\n', b'\n')
    blob = hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest()
    if blob != 'c0b5e1d7dd1396af7721d860ad8e1cb9800e4fe3':
        raise SystemExit('Pinned LP5 encoder source changed')
    text = raw.decode()
    start = text.index('static __always_inline u16 pmu_pack_phase_desc(')
    end = text.index('/**\n * pmu_cal_multi_branch_dispatch()', start)
    extract = build / 'inferno_encoder_extract.h'
    extract.write_text('/* SPDX-License-Identifier: GPL-2.0+ */\n'
        '/* Copyright (C) 2026 Alastair D\'Silva. Exact public source excerpt for tests. */\n'
        'typedef uint16_t u16;\ntypedef uint32_t u32;\n' + text[start:end])
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I', str(source), '-I', str(build), str(source / 'commands.c'),
               str(source / 'test_commands.c'), '-o', str(build / 'test_commands')]
    logs = []
    for cmd in (command, [str(build / 'test_commands')]):
        print(json.dumps(cmd), flush=True)
        result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        logs.append(json.dumps(cmd) + '\n' + result.stdout)
        (root / 'evidence/lpddr4-pmu-commands-check.log').write_text(''.join(logs))
        if result.returncode:
            raise SystemExit(result.returncode)
    files = [lp4_path, lp5_path, train, vectors, extract, source / 'commands.c',
             source / 'commands.h', source / 'test_commands.c', Path(__file__)]
    report = {
        'checks_complete': True,
        'lp4_mrw_rank_mr_operand_vectors': 2 * 64 * 256,
        'lp4_mrr_rank_mr_vectors': 2 * 64,
        'lp4_mpc_rank_opcode_inputs': 2 * 128,
        'lp5_descriptor_mrw_wire_matches': 128 * 256,
        'lp5_descriptor_mrr_wire_matches': 128,
        'lp5_descriptor_mpc_wire_matches': 256,
        'native_address_undefined_sanitizers_pass': True,
        'inputs': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        'established': 'Public LP4 CA/CS pin encoding; public LP5 descriptor d3/d4 encode CA phases.',
        'type9_lp4_clock_mapping_established': False,
        'type9_lp4_executor_implemented': False,
        'pmu_executable_built': False,
        'hardware_executed': False,
        'deployable': False,
    }
    report_file.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
