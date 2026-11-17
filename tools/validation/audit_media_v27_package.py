#!/usr/bin/env python3
"""Read-only audit of the frozen v27 decoder package against storage-v26.

Run on the build host after the final kernel and external PVR build. Never
installs a package, runs maintainer scripts, loads modules, or accesses a board.
"""
import argparse
import datetime
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from inspect_packed_dtb import parse_fdt

RELEASE = '6.18.63-t736-media-v27'
VERSION = '6.18.63-27'
EXPECTED = {
    'package': '2370636db2b65aa371ab99aa587519e9b7a44ffbdd356426ee978c053b8b203f',
    'dtb': '45012aeb86d657fca568417e831067170d12e224ebe71720c1956989af6f3626',
    'pvr': '981d873348e061af66c030f21af1f050ccb7d4df156ac9abbfcfa7a885a6652d',
}
BASE_RECORD_SHA = '7fcd748956a8832ba5cf82de243dfb3836e81cfa5ad74b08f2c15d8337e143ae'
NEW_NODES = {'/clock-controller@2002a88', '/power-controller@7062000',
             '/video-codec@1c0e000'}
SOURCE_MAP = {
    'boards/clk-t736-ve.c': 'drivers/clk/clk-t736-ve.c',
    'boards/t736-ve-power.c': 'drivers/soc/sunxi/t736-ve-power.c',
    'boards/t736-cedar-wc.c': 'drivers/misc/t736-cedar-wc.c',
    'boards/t736-media-v27.dtsi': 'arch/arm64/boot/dts/allwinner/t736-media-v27.dtsi',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def file_record(path, root):
    return {'path': path.relative_to(root).as_posix(), 'bytes': path.stat().st_size,
            'sha256': sha(path)}


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def config(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines()
                if line.startswith('CONFIG_'))


def cells(value):
    require(len(value) % 4 == 0, 'Unaligned FDT cells')
    return list(struct.unpack('>' + 'I' * (len(value) // 4), value))


def refs(nodes, path, prop):
    handles = {}
    for node, properties in nodes.items():
        for key in ('phandle', 'linux,phandle'):
            if key in properties:
                value = cells(properties[key])
                require(len(value) == 1 and value[0] not in (0, 0xffffffff), 'Bad phandle')
                require(value[0] not in handles or handles[value[0]] == node, 'Duplicate phandle')
                handles[value[0]] = node
    count_key = {'clocks': '#clock-cells', 'resets': '#reset-cells',
                 'power-domains': '#power-domain-cells', 'dmas': '#dma-cells',
                 'iommus': '#iommu-cells', 'phys': '#phy-cells'}.get(prop)
    words, result = cells(nodes[path][prop]), []
    while words:
        require(words[0] in handles, 'Unknown phandle in ' + path + ':' + prop)
        target = handles[words.pop(0)]
        count = cells(nodes[target][count_key])[0] if count_key else 0
        require(len(words) >= count, 'Truncated phandle arguments')
        result.append([target, words[:count]])
        words = words[count:]
    return result


def audit_dtb(old_path, new_path):
    old, oh = parse_fdt(old_path.read_bytes())
    new, nh = parse_fdt(new_path.read_bytes())
    require(set(new) - set(old) == NEW_NODES and not set(old) - set(new), 'Unexpected DT node delta')
    require({k: v for k, v in oh.items() if k != 'total_size'} ==
            {k: v for k, v in nh.items() if k != 'total_size'}, 'DT header/reservations changed')
    harmless = []
    reference_names = {'clocks', 'resets', 'power-domains', 'dmas', 'iommus',
                       'phys', 'interrupt-parent'}
    for path, properties in old.items():
        for prop in set(properties) | set(new[path]):
            before, after = properties.get(prop), new[path].get(prop)
            if before == after:
                continue
            if prop in ('phandle', 'linux,phandle'):
                require(after is not None and len(after) == 4, 'Removed/malformed phandle')
                harmless.append({'path': path, 'property': prop, 'kind': 'phandle-number-or-addition'})
            elif prop in reference_names or prop.endswith('-supply') or re.fullmatch(r'pinctrl-\d+', prop):
                require(before is not None and after is not None and
                        refs(old, path, prop) == refs(new, path, prop), 'DT reference changed: ' + path + ':' + prop)
                harmless.append({'path': path, 'property': prop, 'kind': 'same-path-reference-renumbering'})
            else:
                raise ValueError('Existing DT property changed: ' + path + ':' + prop)
    cc, pd, ve = '/clock-controller@2002a88', '/power-controller@7062000', '/video-codec@1c0e000'
    exact = {
        cc: {'compatible': b'allwinner,t736-ve-ccu\0', '#clock-cells': [1], '#reset-cells': [1],
             'reg': [0, 0x02002a88, 0, 8, 0, 0x02002000, 0, 4, 0, 0x020020a0, 0, 4,
                     0, 0x020025c0, 0, 4, 0, 0x020025e0, 0, 4, 0, 0x020025e4, 0, 4, 0, 0x020025b4, 0, 4]},
        pd: {'compatible': b'allwinner,t736-ve-power\0', '#power-domain-cells': [0],
             'reg': [0, 0x07062000, 0, 0x1000, 0, 0x03910404, 0, 4, 0, 0x03910604, 0, 4]},
        ve: {'compatible': b'allwinner,t736-cedar-wc\0', 'reg': [0, 0x01c0e000, 0, 0x2000],
             'interrupts': [0, 49, 4], 'status': b'okay\0'},
    }
    for path, properties in exact.items():
        for key, value in properties.items():
            actual = new[path].get(key)
            require(actual is not None and (cells(actual) if isinstance(value, list) else actual) == value,
                    'Unexpected media DT property: ' + path + ':' + key)
    require('iommus' not in new[ve] and 'dma-coherent' not in new[ve], 'Decoder must use private noncoherent heap')
    require(refs(new, cc, 'clocks') == [['/dcxo', []], ['/clock-ahb-firmware', []], ['/clock-mbus-firmware', []]], 'Wrong CCU parents')
    require(refs(new, pd, 'clocks') == [['/clock-controller@2002a00', [11]]] +
            [[cc, [i]] for i in (2, 3, 4, 1, 6, 5, 0)], 'Wrong power clocks')
    require(refs(new, pd, 'resets') == [[cc, [0]]] and refs(new, ve, 'resets') == [[cc, [0]]], 'Wrong reset owner')
    require(refs(new, ve, 'clocks') == [[cc, [0]]] and refs(new, ve, 'power-domains') == [[pd, []]], 'Wrong decoder clock/domain')
    return {'added_nodes': sorted(NEW_NODES), 'existing_nodes': len(old),
            'existing_semantics_unchanged': True, 'phandle_only_changes': harmless,
            'new_node_properties_hex': {p: {k: v.hex() for k, v in new[p].items()} for p in sorted(NEW_NODES)}}


def audit_package(package, expected_files):
    found, inventory = {}, {}
    with subprocess.Popen(['dpkg-deb', '--fsys-tarfile', str(package)], stdout=subprocess.PIPE) as proc:
        with tarfile.open(fileobj=proc.stdout, mode='r|') as archive:
            for member in archive:
                name = member.name.removeprefix('./')
                require(not PurePosixPath(name).is_absolute() and '..' not in PurePosixPath(name).parts, 'Unsafe package path')
                require(name not in inventory, 'Duplicate package entry: ' + name)
                entry = {'type': member.type.decode('ascii'), 'mode': member.mode, 'bytes': member.size}
                if member.issym() or member.islnk():
                    entry['linkname'] = member.linkname
                if member.isfile():
                    with archive.extractfile(member) as stream:
                        entry['sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
                    if name in expected_files:
                        require(entry['sha256'] == sha(expected_files[name]), 'Package/build mismatch: ' + name)
                        found[name] = entry
                inventory[name] = entry
        require(proc.wait() == 0, 'dpkg-deb read failed')
    require(set(found) == set(expected_files), 'Missing package artifact')
    control = {key: run('dpkg-deb', '-f', str(package), key) for key in ('Package', 'Version', 'Architecture')}
    require(control == {'Package': 'linux-image-' + RELEASE, 'Version': VERSION, 'Architecture': 'arm64'}, 'Unexpected package control')
    return {'control': control, 'matched_build_members': found, 'payload_manifest': inventory,
            'maintainer_scripts_executed': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    out, source, base = [root / p for p in ('build/linux-media-v27', 'sources/work/linux-media-v27', 'build/linux-storage-v26')]
    package = root / f'build/linux-image-{RELEASE}_{VERSION}_arm64.deb'
    pvr = root / 'build/pvr-bxm-media-v27/target_aarch64/kbuild/pvrsrvkm.ko'
    dtb = out / 'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb'
    for key, path in {'package': package, 'pvr': pvr, 'dtb': dtb}.items():
        require(sha(path) == EXPECTED[key], 'Final build hash mismatch: ' + key)
    baseline_record = root / 'evidence/storage-v26-build-20261116.json'
    require(sha(baseline_record) == BASE_RECORD_SHA, 'Frozen v26 evidence changed')
    baseline = json.loads(baseline_record.read_text())
    original = {**baseline['files'], baseline['package']['path']: baseline['package']['sha256']}
    for name, expected in original.items():
        require(sha(root / name) == expected, 'Frozen v26 input changed: ' + name)
    require((out / 'include/config/kernel.release').read_text().strip() == RELEASE, 'Wrong release')
    old_cfg, new_cfg = config(base / '.config'), config(out / '.config')
    changes = {k: [old_cfg.get(k, 'n'), new_cfg.get(k, 'n')] for k in sorted(set(old_cfg) | set(new_cfg))
               if old_cfg.get(k, 'n') != new_cfg.get(k, 'n')}
    require(changes == {'CONFIG_LOCALVERSION': ['"-t736-storage-v26"', '"-t736-media-v27"'],
                        'CONFIG_DMABUF_HEAPS': ['n', 'y'], 'CONFIG_VIDEO_T736_CEDAR_WC': ['n', 'y']}, 'Unexpected configuration delta')
    for disabled in ('CONFIG_AW_IOMMU', 'CONFIG_DMABUF_HEAPS_SYSTEM', 'CONFIG_DMABUF_HEAPS_CMA'):
        require(new_cfg.get(disabled, 'n') == 'n', 'Unexpected enabled component: ' + disabled)
    require(new_cfg.get('CONFIG_CMA_SIZE_MBYTES') == '256', 'CMA size changed')
    bound_sources = []
    for board_name, tree_name in SOURCE_MAP.items():
        board, tree = root / board_name, source / tree_name
        require(board.read_bytes() == tree.read_bytes(), 'Workspace/tree source mismatch: ' + board_name)
        bound_sources.append(file_record(board, root))
        if board.suffix == '.c':
            obj = out / Path(tree_name).with_suffix('.o')
            require(obj.stat().st_mtime_ns >= tree.stat().st_mtime_ns, 'Object predates source: ' + tree_name)
            bound_sources.append(file_record(obj, root))
    mmc = 'drivers/mmc/host/t736_mmc_pio.c'
    require(sha(source / mmc) == sha(root / 'sources/work/linux-storage-v26' / mmc) ==
            baseline['files']['boards/t736_mmc_cached_bounce_draft.c'], 'v26 MMC source changed')
    symbol_text = run('aarch64-linux-gnu-nm', '-n', str(out / 'vmlinux'))
    symbols = {line.split()[-1] for line in symbol_text.splitlines() if line.split()}
    required_symbols = ['ve_ccu_probe', 've_power_probe', 've_probe', 've_heap_allocate',
                        've_buffer_ops', 've_power_driver', 've_ccu_driver', 've_driver', 'dma_heap_add']
    require(all(s in symbols for s in required_symbols), 'Cedar/provider/heap symbols absent from vmlinux')
    vermagic = run('modinfo', '-F', 'vermagic', str(pvr))
    require(vermagic == RELEASE + ' SMP mod_unload aarch64', 'PVR vermagic mismatch')
    require(new_cfg.get('CONFIG_MODVERSIONS', 'n') == 'n', 'Versioned module audit not implemented')
    elf = pvr.read_bytes()
    require(elf[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', elf, 18)[0] == 183, 'PVR not ELF64 AArch64')
    require(gzip.decompress((out / 'arch/arm64/boot/Image.gz').read_bytes()) ==
            (out / 'arch/arm64/boot/Image').read_bytes(), 'Image.gz differs from Image')
    matched = {f'boot/vmlinuz-{RELEASE}': out / 'arch/arm64/boot/Image.gz',
               f'boot/config-{RELEASE}': out / '.config', f'boot/System.map-{RELEASE}': out / 'System.map',
               f'usr/lib/linux-image-{RELEASE}/allwinner/t736-gpu-candidate.dtb': dtb}
    record = {'schema': 1, 'kind': 'media-v27-independent-package-audit',
              'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'status': 'PASS_STATIC_PACKAGE_AUDIT_NOT_HARDWARE_ACCEPTANCE',
              'kernel_release': RELEASE, 'config_changes': changes,
              'package': file_record(package, root), 'pvr': {**file_record(pvr, root), 'vermagic': vermagic},
              'kernel_artifacts': [file_record(out / x, root) for x in ('arch/arm64/boot/Image', 'arch/arm64/boot/Image.gz', '.config', 'System.map', 'Module.symvers', 'vmlinux')],
              'source_and_objects': bound_sources, 'linked_vmlinux_symbols': required_symbols,
              'dtb': {**file_record(dtb, root), **audit_dtb(base / 'arch/arm64/boot/dts/allwinner/t736-gpu-candidate.dtb', dtb)},
              'frozen_v26_hashes_rechecked': original, 'frozen_v26_unchanged': True,
              **audit_package(package, matched),
              'board_accessed': False, 'module_loaded': False, 'codec_accepted': False,
              'limits': ['Source/object timestamp, exact copied sources and linked symbols checked; no reproducible rebuild performed.',
                         'PVR vermagic and ELF architecture checked; CONFIG_MODVERSIONS is disabled as in v26.',
                         'No runtime decode, DMA lifetime, stability, encoding or browser acceptance.']}
    target = args.output or root / 'evidence/media-v27-independent-package-audit-20261117.json'
    target.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': record['status'], 'evidence': str(target), 'sha256': sha(target),
                      'v26_unchanged': True, 'config_changes': changes, 'added_dt_nodes': sorted(NEW_NODES),
                      'pvr_vermagic': vermagic}, ensure_ascii=False))


if __name__ == '__main__':
    main()
