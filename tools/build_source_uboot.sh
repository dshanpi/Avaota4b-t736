#!/bin/sh
# Rebuild the captured source-v6 U-Boot in a new isolated directory.
# Does not overwrite the deployed/pinned payload or access the board or Tina.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
archive="$root/sources/archives/u-boot-upstream.tar.gz"
work="$root/build/source-uboot-v6-rebuild"
src="$work/source"
out="$work/output"
patchfile="$root/patches/u-boot/t736-source-boot.patch"
config="$root/config/u-boot-source-v6.config"
test ! -e "$work"

# The archive is an external input exported from the locked official commit.
# Check it before extraction and verify the captured patch/config as a pair.
python3 - "$root" "$archive" <<'PY'
from pathlib import Path
import hashlib, json, sys, tarfile
root, archive = map(Path, sys.argv[1:])
record = json.loads((root / 'evidence/source-uboot-backup-audit-20261115.json').read_text())
for path, expected in [
    (archive, record['upstream_archive_sha256']),
    (root / 'patches/u-boot/t736-source-boot.patch', record['patch_sha256']),
    (root / 'config/u-boot-source-v6.config', record['config_sha256']),
]:
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, path
with tarfile.open(archive) as package:
    assert package.pax_headers.get('comment') == record['upstream_commit']
PY

mkdir -p "$src" "$out"
tar -xf "$archive" -C "$src" --no-same-owner
(cd "$src" && git apply --check "$patchfile" && git apply "$patchfile")
cp "$config" "$out/.config"

# SOURCE_DATE_EPOCH uses UTC in upstream U-Boot. The installed build originally
# used local -0500 time, so this clean rebuild does not promise the same hash.
export SOURCE_DATE_EPOCH=1794647291
make -C "$src" O="$out" ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
cmp "$config" "$out/.config"
make -C "$src" O="$out" ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j"${T736_JOBS:-8}"
test -s "$out/u-boot.bin"
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import hashlib, json, sys
root, out = map(Path, sys.argv[1:])
audit = json.loads((root / 'evidence/source-uboot-backup-audit-20261115.json').read_text())
digest = hashlib.sha256((out / 'u-boot.bin').read_bytes()).hexdigest()
record = {
    'upstream_commit': audit['upstream_commit'],
    'config_sha256': audit['config_sha256'],
    'patch_sha256': audit['patch_sha256'],
    'rebuilt_payload_sha256': digest,
    'deployed_payload_sha256': audit['deployed_payload_sha256'],
    'matches_deployed_bytes': digest == audit['deployed_payload_sha256'],
    'source_date_epoch': 1794647291,
    'hardware_tested': False,
    'note': 'Fresh candidate only. The deployment packer remains pinned to the board-tested payload; review any changed hash before packaging.',
}
(out / 'rebuild-record.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
PY
