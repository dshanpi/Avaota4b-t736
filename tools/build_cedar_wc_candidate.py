#!/usr/bin/env python3
"""Build a private Cedar2.0 WC-heap candidate; no board or Tina mutation.

This candidate requires the matching, audited t736-ve WC-only DMA heap.
It never falls back to system/reserved heaps. Proprietary output stays private.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function_replace(text, signature, body):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[:brace] + '{\n' + body.strip() + '\n}' + text[end:]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--tina', type=Path, default=Path('/home/ubuntu/t736-aiot/tina'))
    ap.add_argument('--bridge', type=Path, default=Path('/home/ubuntu/t736-mainline/build/cedar-bridge-package-v2'))
    args = ap.parse_args()
    work, tina, bridge = args.work.resolve(), args.tina.resolve(), args.bridge.resolve()
    if work.exists() or work == tina or tina in work.parents:
        ap.error('work must be a new directory outside Tina')
    origin = tina / 'platform/allwinner/multimedia/libcedarc_v2.0'
    sdk = tina / 'out/t736/demo_aiot/buildroot/buildroot/host'
    vendor = bridge / 'cedar/usr/lib/aarch64-linux-gnu'
    source = work / 'src'
    work.mkdir(parents=True)
    source.mkdir()
    inputs = {}
    for name in ('base', 'include', 'memory'):
        shutil.copytree(origin / name, source / name)
        for p in (origin / name).rglob('*'):
            if p.is_file():
                inputs[str(p.relative_to(origin))] = sha(p)
    assert sha(vendor / 'libVE.so') == '3af49a5eb24f6832518a858274fb206d60e490985bff02f5d5be36d053f65be4'
    for name in ('sc_interface.h', 'veInterface.h', 'vencoder.h', 'vdecoder.h', 'memoryAdapter.h'):
        assert (origin / 'include' / name).read_bytes() == (bridge / 'cedar/usr/include' / name).read_bytes()
    for name in ('config.h',):
        p = tina / 'out/t736/demo_aiot/buildroot/buildroot/build/libcedarc' / name
        shutil.copy2(p, source / name)
        inputs[name] = sha(p)
    base = source / 'base/CdcIonUtil.c'
    text = base.read_text(encoding='utf-8', errors='surrogateescape')
    text = function_replace(text, 'int CdcIonOpen(void)', r'''
    /* This dedicated build requires dma_alloc_wc/dma_mmap_wc, not a name alias. */
    int fd = open("/dev/dma_heap/t736-ve", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        loge("T736_WC: required /dev/dma_heap/t736-ve unavailable: %s", strerror(errno));
    return fd;
''')
    base.write_text(text, encoding='utf-8', errors='surrogateescape')
    mem = source / 'memory/ionMemory/ionAlloc.c'
    text = mem.read_text(encoding='utf-8', errors='surrogateescape')
    text = text.replace('#include <linux/dma-buf.h>', '#include <linux/dma-buf.h>\n#include <stdint.h>\n#include <errno.h>')
    text = function_replace(text, 'static int get_phy_addr(', r'''
    struct user_iommu_param param = { .fd = nBufFd, .iommu_addr = 0 };
    UNUSA_PARAM(nIonFd);
    *pAddr = 0;
    if (CdcVeGetIommuAddr(g_context->ve_ops, g_context->ve_self, &param) != 0)
        return -1;
    if (!param.iommu_addr || (param.iommu_addr & 0xff)) {
        CdcVeFreeIommuAddr(g_context->ve_ops, g_context->ve_self, &param);
        errno = ERANGE;
        return -1;
    }
    *pAddr = param.iommu_addr;
    return 0;
''')
    text = function_replace(text, 'static int ion_create_context(', r'''
    if (!veops || !veself) { errno = EINVAL; return -1; }
    g_context = calloc(1, sizeof(*g_context));
    if (!g_context) return -1;
    g_context->fd = CdcIonOpen();
    if (g_context->fd < 0) goto fail;
    g_context->ve_ops = veops;
    g_context->ve_self = veself;
    g_context->phyOffset = CdcVeGetPhyOffset(veops, veself);
    fprintf(stderr, "T736_WC: version=0x%llx phy_offset=0x%x heap=t736-ve cache=WC barriers=dmb-osh\n",
            (unsigned long long)CdcVeGetIcVeVersion(veops, veself), g_context->phyOffset);
    if (g_context->phyOffset != 0) {
        close(g_context->fd);
        errno = ERANGE;
        goto fail;
    }
    AW_MEM_INIT_LIST_HEAD(&g_context->list);
    g_context->ref_cnt = 1;
    return 0;
fail:
    free(g_context);
    g_context = NULL;
    return -1;
''')
    text = function_replace(text, 'static int ion_alloc_get_phy(', r'''
    unsigned long address = 0;
    int ret;
    if (!phyaddr) { errno = EINVAL; return -1; }
    *(unsigned int *)phyaddr = 0;
    pthread_mutex_lock(&g_mutex_alloc);
    if (!g_context) { pthread_mutex_unlock(&g_mutex_alloc); return -1; }
    ret = get_phy_addr(g_context->fd, dmafd, &address);
    if (!ret) *(unsigned int *)phyaddr = (unsigned int)address;
    pthread_mutex_unlock(&g_mutex_alloc);
    return ret;
''')
    # Replace the definition, not the earlier forward declaration.
    text = function_replace(text, 'static void ion_alloc_flush_cache(void*', r'''
    buffer_node *node;
    uintptr_t start = (uintptr_t)startAddr;
    int found = 0;
    static unsigned long barrier_count;
    pthread_mutex_lock(&g_mutex_alloc);
    if (g_context && startAddr && size > 0) {
        aw_mem_list_for_each_entry(node, &g_context->list, i_list) {
            uintptr_t base = (uintptr_t)node->vir;
            if (start >= base && start - base <= node->size &&
                (size_t)size <= node->size - (start - base)) {
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        fprintf(stderr, "T736_WC: rejecting flush outside owned WC mapping: %p size=%d\n", startAddr, size);
        pthread_mutex_unlock(&g_mutex_alloc);
        abort(); /* The legacy callback is void: fail closed, never continue unsynchronized. */
    }
    /* No WB alias exists in this heap. Order WC CPU accesses vs VE/MMIO; not a cache invalidate. */
    __asm__ __volatile__("dmb osh" ::: "memory");
    if (++barrier_count == 1)
        fprintf(stderr, "T736_WC: first validated WC ordering barrier\n");
    pthread_mutex_unlock(&g_mutex_alloc);
''')
    text = function_replace(text, 'static void* ion_alloc_palloc_base(', r'''
    int fd = -1, mapped = 0;
    unsigned char *virt = NULL;
    unsigned long address = 0;
    buffer_node *node = NULL;
    struct user_iommu_param param = {0};
    UNUSA_PARAM(veOps); UNUSA_PARAM(pVeopsSelf); UNUSA_PARAM(bIsCache);
    pthread_mutex_lock(&g_mutex_alloc);
    if (!g_context || size <= 0) goto fail;
    fd = CdcDmaheapAllocFd(g_context->fd, size);
    if (fd < 0) goto fail;
    if (CdcIonMmap(fd, size, &virt) < 0) { virt = NULL; goto fail; }
    if (get_phy_addr(g_context->fd, fd, &address) != 0) goto fail;
    mapped = 1;
    if ((uint64_t)address + (uint64_t)size - 1 > UINT32_MAX) { errno = ERANGE; goto fail; }
    node = calloc(1, sizeof(*node));
    if (!node) goto fail;
    node->phy = address;
    node->vir = virt;
    node->user_virt = virt;
    node->size = size;
    node->dma_buf_fd = fd;
    memset(virt, 0xff, size);
    __asm__ __volatile__("dmb osh" ::: "memory");
    aw_mem_list_add_tail(&node->i_list, &g_context->list);
    pthread_mutex_unlock(&g_mutex_alloc);
    return virt;
fail:
    if (mapped) {
        param.fd = fd; param.iommu_addr = address;
        CdcVeFreeIommuAddr(g_context->ve_ops, g_context->ve_self, &param);
    }
    if (virt) CdcIonMunmap(size, virt);
    if (fd >= 0) close(fd);
    free(node);
    pthread_mutex_unlock(&g_mutex_alloc);
    return NULL;
''')
    old = '        aw_mem_list_del(pos);\n        free(tmp);'
    assert text.count(old) == 1
    text = text.replace(old, '''        struct user_iommu_param param = { .fd = tmp->dma_buf_fd, .iommu_addr = tmp->phy };
        if (CdcVeFreeIommuAddr(g_context->ve_ops, g_context->ve_self, &param) != 0)
            fprintf(stderr, "T736_WC: release-time unmap failed for fd=%d\\n", tmp->dma_buf_fd);
        CdcIonMunmap(tmp->size, tmp->vir);
        close(tmp->dma_buf_fd);
        aw_mem_list_del(pos);
        free(tmp);''')
    mem.write_text(text, encoding='utf-8', errors='surrogateescape')
    bundle = work / 't736-cedar-wc-v1'
    shutil.copytree(bridge / 't736-cedar-bridge-v2', bundle)
    (bundle/'manifest.json').rename(bundle/'BASELINE-BRIDGE-MANIFEST.json')
    shutil.copy2(Path(__file__), work / 'build_cedar_wc_candidate.py')
    compiler = sdk / 'bin/aarch64-none-linux-gnu-gcc'
    common = [str(compiler), '-O2', '-g', '-fPIC', '-shared', '-Wall', '-D_GNU_SOURCE',
              '-DCONF_KERNEL_VERSION_6_6', '-D__LINUX__', '-DCONF_USE_IOMMU',
              '-DCONF_AUTO_SCALE_OVER_4K', '-DCONF_AW_AFBC_MODE=0', '-DCONF_VE_MULTIPLE_OUTPUT',
              '-DHAVE_CONFIG_H', '-I'+str(source), '-I'+str(source/'include'),
              '-I'+str(source/'base/include'), '-I'+str(source/'base/filesink/include'),
              '-Wl,--no-undefined']
    base_sources = ['CdcLog.c', 'CdcSysinfo.c', 'CdcMessageQueue.c', 'CdcIonUtil.c', 'CdcMalloc.c',
                    'cdcIniparser/cdcDictionary.c', 'cdcIniparser/cdcIniparserapi.c', 'cdcIniparser/cdcIniparser.c',
                    'CdcSinkInterface.c', 'filesink/CdcBSSink.c', 'filesink/CdcPicSink.c',
                    'filesink/SinkMd5.c', 'CdcTimeUtil.c']
    commands = [common + [str(source/'base'/p) for p in base_sources] +
                ['-Wl,-soname,libcdc_base.so', '-lpthread', '-lm', '-lrt', '-o', str(bundle/'lib/libcdc_base.so')],
                common + [str(source/'memory/memoryAdapter.c'), str(mem), '-L'+str(bundle/'lib'),
                '-Wl,-rpath-link,'+str(bundle/'lib'), '-Wl,-soname,libMemAdapter.so', '-lcdc_base', '-lVE',
                '-lpthread', '-lm', '-lrt', '-o', str(bundle/'lib/libMemAdapter.so')]]
    for i, command in enumerate(commands):
        with (work/f'compile-{i}.log').open('w') as out:
            subprocess.run(command, stdout=out, stderr=subprocess.STDOUT, check=True)
    abi = {}
    for name in ('libcdc_base.so', 'libMemAdapter.so'):
        def exported(p):
            data = subprocess.check_output(['readelf', '--dyn-syms', '-W', str(p)], text=True)
            return {l.split()[-1].split('@')[0] for l in data.splitlines()
                    if (' GLOBAL ' in l or ' WEAK ' in l) and ' UND ' not in l}
        missing = sorted(exported(vendor/name) - exported(bundle/'lib'/name))
        abi[name] = {'missing_exported_symbols': missing, 'sha256': sha(bundle/'lib'/name)}
        assert not missing, (name, missing)
    metadata = {'schema': 1, 'status': 'COMPILED_ABI_CHECKED_HARDWARE_PENDING',
                'fixed_heap': '/dev/dma_heap/t736-ve', 'heap_fallback': False,
                'required_contract': 'same VE device dma_alloc_wc/dma_mmap_wc, no WB aliases/bounce, 32bit contiguous DMA, only own heap import',
                'cache_contract': 'owned allocation range checked; dmb osh plus compiler memory clobber; invalid range aborts; no cache flush claimed',
                'phy_offset': 'runtime zero-only guard; actual libVE version printed',
                'libVE_unchanged_sha256': sha(vendor/'libVE.so'), 'libVE_dma_ioctl': ['0x502', '0x503'],
                'source_inputs': inputs, 'commands': commands, 'abi': abi,
                'builder_sha256': sha(Path(__file__)), 'no_board_execution': True,
                'modified_sources': {str(p.relative_to(source)): sha(p) for p in (base, mem)}}
    (bundle/'WC-MANIFEST.json').write_text(json.dumps(metadata, indent=2)+'\n')
    (bundle/'README.txt').write_text('Private experimental T736 Cedar2.0 WC-only heap bundle.\n'
        'Requires matching /dev/dma_heap/t736-ve kernel implementation; no fallback.\n'
        'Uses original libVE DMA ioctl 0x502/0x503, runtime zero phy_offset guard.\n'
        'Owned WC mappings use explicit dmb osh ordering, not cache invalidation.\n'
        'Invalid flush range aborts. Hardware decode and coherent visibility remain unverified.\n'
        'Never publish proprietary libraries. No automatic player/browser installation.\n')
    inventory = {'schema': 1, 'status': metadata['status'], 'kind': 't736-cedar-wc-v1',
                 'files': [{'path': str(p.relative_to(bundle)), 'bytes': p.stat().st_size, 'sha256': sha(p)}
                           for p in sorted(bundle.rglob('*')) if p.is_file()]}
    (bundle/'manifest.json').write_text(json.dumps(inventory, indent=2)+'\n')
    archive = work/(bundle.name+'.tar.gz')
    with tarfile.open(archive,'w:gz') as out:
        out.add(bundle,arcname=bundle.name)
    record = {'package':str(archive),'bytes':archive.stat().st_size,'sha256':sha(archive),
              'manifest_sha256':sha(bundle/'manifest.json'),
              'wc_manifest_sha256':sha(bundle/'WC-MANIFEST.json'),'abi':abi}
    (work/'build-record.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(record))


if __name__ == '__main__':
    main()
