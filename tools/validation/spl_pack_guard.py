"""Pinned historical SPL provenance only; retired SPL v5 is always rejected."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct

DDR_SHA256 = '5361d148fa097ccc14e15a173523783d0eb1a71969bc76d57eb8e1229ddc6d90'
LEGACY_SPL = {
    'v3': '7a3b30d3a0349e2b62d06fa07699cf1284af1e60a5704d4f1330aaf5011532db',
    'v4': 'a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea',
}
RETIRED_V5 = 'SPL v5 is retired; use pinned SPL v4 and a separately accepted source U-Boot'


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def project_file(root, relative):
    name = PurePosixPath(relative)
    require(isinstance(relative, str) and not name.is_absolute() and '..' not in name.parts,
            'Expected a project-relative file path')
    path = (Path(root) / relative).resolve(strict=True)
    require(path.is_relative_to(Path(root).resolve()) and path.is_file(), 'File leaves project or is not regular')
    return path


def resolve_variant(image_variant, selected=None):
    require(selected != 'v5', RETIRED_V5)
    require(selected in (None, 'v4'), 'Unsupported SPL variant')
    require(image_variant in ('v3', 'v4', 'v5', 'v6', 'v7', 'v8', 'v9', 'v10'),
            'Unsupported source image revision')
    if image_variant == 'v3':
        require(selected is None, 'Legacy source-v3 keeps its original SPL; select a newer image variant')
        return 'v3'
    return selected or 'v4'


def load_spl(root, variant):
    require(variant != 'v5', RETIRED_V5)
    require(variant in ('v3', 'v4'), 'Unsupported SPL build')
    directory = f'build/emmc-spl-{variant}'
    record_path = project_file(root, directory + '/build-record.json')
    record = json.loads(record_path.read_text())
    # The original v3 record predates the variant field. Its fixed directory,
    # full header checks and pinned binary hash still bind that historical build.
    recorded_variant = record.get('variant', 'v3' if variant == 'v3' else None)
    require(record.get('kind') == 'source-emmc-spl-with-authorized-ddr-library'
            and recorded_variant == variant, 'SPL build identity mismatch')
    require(record.get('vendor_ddr_sha256') == DDR_SHA256, 'Unexpected DDR library provenance')
    require(record.get('entry') == '0x47000' and record.get('window_end') == '0x7fc00', 'Unexpected SPL SRAM layout')
    packed = project_file(root, directory + '/emmc-spl.bin')
    data = packed.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    require(len(data) == record.get('packed_bytes') and digest == record.get('packed_sha256'), 'SPL differs from build record')
    require(64 <= len(data) <= 0x7fc00 - 0x47000 and len(data) % 4 == 0, 'Invalid SPL length')
    require(data[4:12] == b'eGON.BT0' and struct.unpack_from('<I', data, 16)[0] == len(data), 'Invalid eGON header')
    checksum = struct.unpack_from('<I', data, 12)[0]
    checked = bytearray(data)
    struct.pack_into('<I', checked, 12, 0x5f0a6c39)
    require(sum(struct.unpack('<%dI' % (len(data) // 4), checked)) & 0xffffffff == checksum, 'Invalid eGON checksum')
    require(int(record.get('egon_checksum', '0'), 0) == checksum, 'eGON checksum differs from build record')
    require(record.get('sources') and all(re.fullmatch('[0-9a-f]{64}', value) for value in record['sources'].values()),
            'Missing SPL source hash provenance')
    # Shared sources contain later experiments. Preserve the historical record
    # without claiming those current files reproduce the pinned v3/v4 binary.
    # No experimental SPL path is accepted, even with a forged acceptance file.
    require(digest == LEGACY_SPL[variant], 'Historical SPL differs from the verified fallback')
    provenance = {'variant': variant, 'build_record': str(record_path.relative_to(Path(root).resolve())),
                  'build_record_sha256': sha(record_path), 'packed_sha256': digest,
                  'packed_bytes': len(data), 'egon_checksum': hex(checksum),
                  'vendor_ddr_sha256': DDR_SHA256, 'sources': record['sources'],
                  'current_sources_verified': False,
                  'source_validation': 'historical source record plus pinned verified SPL',
                  'cpu_clock': record.get('cpu_clock')}
    return data, provenance


def validate_cpu_acceptance(root, path, spl, profile):
    """Retired API: a runtime A76 trial cannot authorize the abandoned SPL v5.

    Kept as an explicit rejection for callers carrying the old interface.
    U-Boot candidate validation and final-image acceptance are separate tasks.
    """
    raise RuntimeError(RETIRED_V5)
