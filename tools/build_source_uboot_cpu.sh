#!/bin/sh
# Isolated A76 CPU candidate. Never overwrites the source-v6 payload or SPL.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
archive="$root/sources/archives/u-boot-upstream.tar.gz"
work="$root/build/source-uboot-cpu-b1"
src="$work/source"
out="$work/output"
test ! -e "$work"
python3 - "$root" "$archive" <<'PY'
from pathlib import Path
import hashlib, json, sys, tarfile
root, archive = map(Path, sys.argv[1:])
record = json.loads((root/'evidence/source-uboot-backup-audit-20261115.json').read_text())
for path, expected in [
    (archive, record['upstream_archive_sha256']),
    (root/'patches/u-boot/t736-source-boot.patch', record['patch_sha256']),
    (root/'config/u-boot-source-v6.config', record['config_sha256']),
]:
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, path
with tarfile.open(archive) as package:
    assert package.pax_headers.get('comment') == record['upstream_commit']
PY
mkdir -p "$src" "$out"
tar -xf "$archive" -C "$src" --no-same-owner
(cd "$src" && git apply --check "$root/patches/u-boot/t736-source-boot.patch" && git apply "$root/patches/u-boot/t736-source-boot.patch")
(cd "$src" && git apply --check "$root/patches/u-boot/t736-cpu-b1.patch" && git apply "$root/patches/u-boot/t736-cpu-b1.patch")
python3 - "$root" "$src" "$out" <<'PY'
from pathlib import Path
import hashlib, json, sys
root, src, out = map(Path, sys.argv[1:])
inputs = [
    'patches/u-boot/t736-source-boot.patch', 'patches/u-boot/t736-cpu-b1.patch',
    'config/u-boot-source-v6.config', 'boards/t736-uboot-cpuclock.c',
    'spl/cpuclock/cpuclock.c', 'spl/cpuclock/cpuclock.h',
    'spl/cpuclock/test_cpuclock.c', 'spl/early/twi.c', 'spl/early/twi.h',
    'tools/build_source_uboot_cpu.sh',
]
record = {name: hashlib.sha256((root/name).read_bytes()).hexdigest() for name in inputs}
(out/'input-sha256.json').write_text(json.dumps(record, indent=2)+'\n')
for name in ['boards/t736-uboot-cpuclock.c', 'spl/cpuclock/cpuclock.c',
             'spl/cpuclock/cpuclock.h', 'spl/early/twi.c', 'spl/early/twi.h']:
    data = (root/name).read_text().replace('#include <stdint.h>',
        '#include <linux/types.h>\n#ifndef UINT32_C\n#define UINT32_C(x) x##U\n#endif')
    (src/'board/t736/bringup'/Path(name).name).write_text(data)
config = (root/'config/u-boot-source-v6.config').read_text()
assert '# CONFIG_BOARD_EARLY_INIT_R is not set' in config
config = config.replace('# CONFIG_BOARD_EARLY_INIT_R is not set', 'CONFIG_BOARD_EARLY_INIT_R=y')
(out/'.config').write_text(config)
(out/'expected.config').write_text(config)
PY
# Fault cases and both observed N-latch timings are verified before compiling.
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$root/spl/cpuclock" \
    "$root/spl/cpuclock/cpuclock.c" "$root/spl/cpuclock/test_cpuclock.c" -o "$out/test_cpuclock"
"$out/test_cpuclock"
export SOURCE_DATE_EPOCH=1794647291
make -C "$src" O="$out" ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
cmp "$out/expected.config" "$out/.config"
make -C "$src" O="$out" ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j"${T736_JOBS:-8}"
test -s "$out/u-boot.bin"
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import hashlib, json, sys
root, out = map(Path, sys.argv[1:])
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
inputs = json.loads((out/'input-sha256.json').read_text())
for name, sha in inputs.items():
    assert digest(root/name) == sha, f'input changed during build: {name}'
audit = json.loads((root/'evidence/source-uboot-backup-audit-20261115.json').read_text())
record = {
    'variant': 'cpu-b1', 'upstream_commit': audit['upstream_commit'],
    'source_date_epoch': 1794647291, 'sources_sha256': inputs,
    'config_sha256': digest(out/'.config'), 'payload_sha256': digest(out/'u-boot.bin'),
    'elf_sha256': digest(out/'u-boot'), 'payload_size': (out/'u-boot.bin').stat().st_size,
    'hook': 'board_early_init_r', 'a76_target_hz': 1014000000,
    'dcdc3_dcdc4_required_selector_uv': 900000, 'hardware_tested': False,
    'host_fault_tests_passed': True,
    'spl_variant': 'v4 unchanged; original pre-DDR recovery preserved',
    'note': 'Candidate only. Runtime v4 passed separately; this exact U-Boot still requires cold-boot acceptance. Existing packer is not changed.',
}
(out/'build-record.json').write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, indent=2))
PY
