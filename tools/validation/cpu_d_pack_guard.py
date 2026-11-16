"""Pure artifact binding for fixed SPL v4 + CPU-D; not board acceptance.

No outputs, device access or subprocesses. A caller must separately check
CPU, GPU, Firefox and final-image evidence before promoting source-v10.
"""
import hashlib
import json
from pathlib import Path
import struct
import zlib

from .spl_pack_guard import DDR_SHA256, project_file, require, sha

SPL_V4_SHA = 'a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea'
SPL_BYTES = 221184
PACKAGE_PATH = 'output/t736-source-boot-package-cpu-d.bin'
PACKAGE_SHA = 'f2ea2157acc3b65b2cc7416dfdba39650e7daa6679693c558fbbdcc30ef9780a'
PACKAGE_BYTES = 325632
MANIFEST_PATH = 'output/t736-source-boot-package-cpu-d.manifest.json'
MANIFEST_SHA = '9bc64b4dab436b998dde31ec020c3b8237fcd195ffbd0f4343ca2c5acf38f462'
BUILD_PATH = 'build/source-uboot-cpu-d/output/build-record.json'
BUILD_SHA = '46f5065b48f1b2a4e18c263d2d84d1a02077267137ca033848d03c2377863d2e'
UBOOT_PATH = 'build/source-uboot-cpu-d/output/u-boot.bin'
CONFIG_PATH = 'build/source-uboot-cpu-d/output/.config'
PAYLOAD_PATHS = ('build/source-handoff-v1/stage64.bin',
                 'output/independent-candidates/t736-bl31.bin', UBOOT_PATH)
BASE_LBA = 24576
SECTORS = 636
LIMIT_LBA = 32800
IMAGES = (
    (24577, 0x47000000, 5040, '44aeb8e56f64f2ccd80a1cd286460eda6d0232f9cc1e82618866826b23298535'),
    (24587, 0x48000000, 53924, '81f53c1d1f571c74bd907317d813a5ed56c64382479caa75edd3cec43ada6dc6'),
    (24693, 0x52000000, 265256, '3ee3a5710c5c13c9ab11b274c6b076447ab346fc7622a1d64b5aaa692f877cdf'),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate_package_bytes(data):
    """Return parsed images only for the exact fixed D bytes and geometry."""
    require(len(data) == PACKAGE_BYTES == SECTORS * 512 and digest(data) == PACKAGE_SHA,
            'CPU-D package size/SHA mismatch')
    words = struct.unpack('<128I', data[:512])
    require(words[:4] == (0x36333754, 0x314c5053, 1, 3), 'CPU-D package signature mismatch')
    require(not any(words[5:8]) and not any(words[20:]), 'CPU-D reserved header fields changed')
    header = bytearray(data[:512])
    struct.pack_into('<I', header, 16, 0)
    require(zlib.crc32(header) == words[4], 'CPU-D header CRC mismatch')
    previous = BASE_LBA + 1
    images = []
    for index, expected in enumerate(IMAGES):
        lba, address, length, crc = words[8 + index * 4:12 + index * 4]
        require((lba, address, length) == expected[:3], 'CPU-D payload geometry mismatch')
        count = (length + 511) // 512
        require(lba == previous and lba + count < LIMIT_LBA, 'CPU-D payload exceeds fixed region')
        offset = (lba - BASE_LBA) * 512
        payload = data[offset:offset + length]
        require(len(payload) == length and digest(payload) == expected[3], 'CPU-D payload SHA mismatch')
        require(zlib.crc32(payload) == crc, 'CPU-D payload CRC mismatch')
        require(not any(data[offset + length:offset + count * 512]), 'CPU-D nonzero sector padding')
        images.append({'sector': lba, 'address': hex(address), 'bytes': length,
                       'crc32': f'{crc:08x}', 'sha256': expected[3], 'offset': offset})
        previous = lba + count
    require(previous == BASE_LBA + SECTORS and len(data) == (previous - BASE_LBA) * 512,
            'CPU-D package trailing bytes')
    return images


def load_cpu_d_package(root, spl_data, spl_provenance):
    """Return (package_bytes, boot_image_items, provenance), all read-only.

    spl_data/provenance are the exact return values from load_spl(root, 'v4').
    This helper validates provenance and bytes but makes no runtime inference.
    """
    root = Path(root).resolve(strict=True)
    require(len(spl_data) == SPL_BYTES and digest(spl_data) == SPL_V4_SHA,
            'CPU-D requires the fixed SPL v4 bytes')
    require(spl_provenance.get('variant') == 'v4'
            and spl_provenance.get('packed_sha256') == SPL_V4_SHA
            and spl_provenance.get('packed_bytes') == SPL_BYTES
            and spl_provenance.get('vendor_ddr_sha256') == DDR_SHA256,
            'CPU-D SPL provenance mismatch')
    data = project_file(root, PACKAGE_PATH).read_bytes()
    images = validate_package_bytes(data)
    manifest_path = project_file(root, MANIFEST_PATH)
    require(sha(manifest_path) == MANIFEST_SHA, 'CPU-D manifest changed')
    manifest = json.loads(manifest_path.read_text())
    require(manifest.get('variant') == 'cpu-d-boot-package'
            and manifest.get('sha256') == PACKAGE_SHA and manifest.get('bytes') == PACKAGE_BYTES
            and manifest.get('sector_bytes') == 512 and manifest.get('start_lba') == BASE_LBA
            and manifest.get('sector_count') == SECTORS
            and manifest.get('end_lba_exclusive') == BASE_LBA + SECTORS
            and manifest.get('required_end_below_lba') == LIMIT_LBA,
            'CPU-D manifest identity/geometry mismatch')
    require(manifest.get('images') == images, 'CPU-D manifest payloads differ')
    require(manifest.get('required_existing_spl') == {'variant': 'v4', 'sha256': SPL_V4_SHA, 'modified': False},
            'CPU-D manifest SPL binding differs')
    require(manifest.get('build_record') == {'path': BUILD_PATH, 'sha256': BUILD_SHA},
            'CPU-D manifest build binding differs')
    build_path = project_file(root, BUILD_PATH)
    require(sha(build_path) == BUILD_SHA, 'CPU-D build record changed')
    build = json.loads(build_path.read_text())
    require(build.get('variant') == 'cpu-d' and build.get('host_fault_tests_passed') is True
            and build.get('a76_startup_hz') == 416000000 and build.get('two_stage') is True
            and build.get('a76_target_hz') == 1014000000 and build.get('a55_target_hz') == 1014000000
            and build.get('a55_d_buffered_trace') is True
            and build.get('a55_d_n_readback_windows') == ['write-N', 'following LOCK_EN']
            and build.get('dcdc3_dcdc4_required_selector_uv') == 900000
            and build.get('cpu0_hotplug_fixed') is False,
            'CPU-D build parameters differ')
    uboot = project_file(root, UBOOT_PATH)
    require(uboot.stat().st_size == build.get('payload_size') == IMAGES[2][2]
            and sha(uboot) == build.get('payload_sha256') == IMAGES[2][3],
            'CPU-D U-Boot differs from fixed build')
    offset = images[2]['offset']
    require(uboot.read_bytes() == data[offset:offset + images[2]['bytes']], 'CPU-D U-Boot differs from package')
    config = project_file(root, CONFIG_PATH)
    require(sha(config) == build.get('config_sha256'), 'CPU-D build configuration changed')
    sources = build.get('sources_sha256')
    require(isinstance(sources, dict) and sources, 'CPU-D source hashes missing')
    # BUILD_SHA pins the complete source map. Check those files as they are now,
    # without claiming to have recompiled or checked the separately recorded ELF.
    for name, expected in sources.items():
        require(sha(project_file(root, name)) == expected, 'CPU-D input changed: ' + name)
    items = []
    for index, image in enumerate(images):
        payload_path = project_file(root, PAYLOAD_PATHS[index])
        require(payload_path.read_bytes() == data[image['offset']:image['offset'] + image['bytes']],
                'CPU-D embedded payload differs from source artifact: ' + PAYLOAD_PATHS[index])
        item = dict(image)
        item['path'] = PAYLOAD_PATHS[index]
        items.append(item)
    provenance = {
        'variant': 'cpu-d', 'validation_scope': 'artifact-binding-only',
        'artifact_binding_verified': True, 'hardware_evidence_checked': False,
        'final_image_cold_boot_validated': False,
        'package_path': PACKAGE_PATH, 'package_sha256': PACKAGE_SHA, 'package_bytes': PACKAGE_BYTES,
        'start_lba': BASE_LBA, 'sector_count': SECTORS, 'end_lba_exclusive': BASE_LBA + SECTORS,
        'manifest_path': MANIFEST_PATH, 'manifest_sha256': MANIFEST_SHA,
        'build_record_path': BUILD_PATH, 'build_record_sha256': BUILD_SHA,
        'uboot_path': UBOOT_PATH, 'uboot_sha256': IMAGES[2][3], 'uboot_bytes': IMAGES[2][2],
        'config_sha256': build['config_sha256'], 'current_sources_verified': True,
        'sources_sha256': sources, 'recorded_elf_rechecked': False,
        'required_spl_variant': 'v4', 'required_spl_sha256': SPL_V4_SHA,
        'stage64_and_tfa_identity_verified': True,
    }
    return data, items, provenance
