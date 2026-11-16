#!/usr/bin/env python3
"""Fixed board boot payload and complete Armbian raw. No legacy boot executable."""
import argparse,hashlib,json,struct,subprocess,zlib
from pathlib import Path
from pack_armbian_raw_v6 import gpt,LAYOUT,SECTORS,file_digest
from validation.spl_pack_guard import load_spl,resolve_variant
from validation.cpu_d_pack_guard import load_cpu_d_package
from validation.source_v10_acceptance import load_source_v10_acceptance
ROOT=Path('/home/ubuntu/t736-mainline')
def read_source_profile_record(root,rootfs,variant):
 assert variant in ('v9','v10'),'Unsupported profile variant'
 record=json.loads((root/('build/source-rootfs-'+variant+'-state/profile-manifest.json')).read_text())
 installed=json.loads(subprocess.run(['debugfs','-R','cat /usr/share/doc/t736-gpu-profile/build-manifest.json',str(rootfs)],check=True,text=True,capture_output=True).stdout)
 assert installed==record,variant+' profile metadata differs from finalized rootfs'
 assert record['variant']==variant and record['desktop_profile'] in ('software','gpu')
 assert record['kernel_release']=={'v9':'6.18.63-t736-gpu-v15','v10':'6.18.63-t736-storage-v16'}[variant]
 return record
def main():
 assert Path(__file__).resolve().parents[1]==ROOT
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--variant',choices=['v3','v4','v5','v6','v7','v8','v9','v10'],default='v3')
 parser.add_argument('--spl-variant',choices=['v4'],help='Only pinned SPL v4 is supported for newer images; abandoned SPL v5 is disabled')
 args=parser.parse_args();variant=args.variant
 spl_variant=resolve_variant(variant,args.spl_variant)
 out=ROOT/'output';spl=ROOT/('build/emmc-spl-'+spl_variant+'/emmc-spl.bin')
 spl_data,spl_provenance=load_spl(ROOT,spl_variant)
 cpu_artifact_binding=None
 if variant=='v10':
  package,items,cpu_artifact_binding=load_cpu_d_package(ROOT,spl_data,spl_provenance)
 else:
  images=[(0x47000000,ROOT/'build/source-handoff-v1/stage64.bin','44aeb8e56f64f2ccd80a1cd286460eda6d0232f9cc1e82618866826b23298535'),(0x48000000,out/'independent-candidates/t736-bl31.bin','81f53c1d1f571c74bd907317d813a5ed56c64382479caa75edd3cec43ada6dc6'),(0x52000000,out/'independent-candidates/u-boot-source-ddr-autoboot.bin','c9b05a7d207781a730c5e12aaae2bdfaf113194f8a3ea4b1e625d330e3f4018b')]
  header=[0]*128;header[:4]=[0x36333754,0x314c5053,1,3];body=bytearray();items=[]
  for i,(a,p,h) in enumerate(images):
   assert file_digest(p)==h;b=p.read_bytes();lba=24577+len(body)//512
   header[8+i*4:12+i*4]=[lba,a,len(b),zlib.crc32(b)];body.extend(b+bytes((-len(b))%512));items.append({'path':str(p.relative_to(ROOT)),'address':hex(a),'sector':lba,'bytes':len(b),'sha256':h})
  h=bytearray(struct.pack('<128I',*header));struct.pack_into('<I',h,16,zlib.crc32(h));package=h+body
  assert 24576+len(package)//512<32800
 rootfs=out/('rootfs-armbian-source-'+variant+'.ext4' if variant in ('v5','v6','v7','v8','v9','v10') else 'rootfs-armbian-source-v1.ext4');assert rootfs.stat().st_size==LAYOUT[3][2]*512
 if variant in ('v7','v8','v9','v10'):
  assert (ROOT/('build/source-rootfs-'+variant+'-state/finalize.complete')).is_file(),'Finish '+variant+' kernel finalization before packing'
  finalized_sha=(out/('rootfs-armbian-source-'+variant+'.ext4.sha256')).read_text().split()[0]
  assert file_digest(rootfs)==finalized_sha,variant+' rootfs changed after finalization'
 subprocess.run(['e2fsck','-fn',str(rootfs)],check=True)
 profile_record=read_source_profile_record(ROOT,rootfs,variant) if variant in ('v9','v10') else None
 cpu_acceptance=load_source_v10_acceptance(ROOT,profile_record,rootfs) if variant=='v10' else None
 bootpack=out/('t736-source-boot-package-source-v10.bin' if variant=='v10' else 't736-source-boot-package-v1.bin')
 if variant=='v10':
  with bootpack.open('xb') as f:f.write(package)
 else:bootpack.write_bytes(package)
 primary,backup=gpt();regions=[('primary-gpt',0,primary),('source-spl',16,spl_data),('source-boot-package',24576,package),('gpt-compatibility-mirror',40960,primary),('backup-gpt',SECTORS-3,backup)]
 raw=out/('t736-armbian-source-'+variant+'.raw.img');records=[]
 with raw.open('xb') as f:
  f.truncate(SECTORS*512)
  for name,sector,b in regions:
   f.seek(sector*512);f.write(b);records.append({'name':name,'sector':sector,'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()})
  f.seek(LAYOUT[3][1]*512)
  with rootfs.open('rb') as src:
   while b:=src.read(8<<20):
    if any(b):f.write(b)
    else:f.seek(len(b),1)
 records.append({'name':'rootfs','sector':LAYOUT[3][1],'bytes':rootfs.stat().st_size,'sha256':file_digest(rootfs)})
 with raw.open('rb') as f:
  for r in records:
   f.seek(r['sector']*512);digest=hashlib.sha256();remain=r['bytes']
   while remain:
    b=f.read(min(remain,8<<20));assert b;digest.update(b);remain-=len(b)
   assert digest.hexdigest()==r['sha256'],r['name']
 subprocess.run(['sfdisk','--verify',str(raw)],check=True)
 manifest={'raw':{'path':raw.name,'bytes':raw.stat().st_size,'sha256':file_digest(raw),'physical_sector_zero':True,'regions':records,'partitions':LAYOUT},'boot_images':items,'vendor_executables':['explicitly authorized sun60iw2 DDR library and embedded DDR training only'],'vendor_ddr_sha256':'5361d148fa097ccc14e15a173523783d0eb1a71969bc76d57eb8e1229ddc6d90','boot_chain':'source eGON SPL + authorized DDR -> source TF-A -> source U-Boot -> Linux -> Armbian','source_spl_build_record':'build/emmc-spl-v3/build-record.json','cold_boot_validated':False,'loader':'output/t736-tina-fes-loader.img (RAM only)','credential':'User authorized agent-chosen root password; secret stored outside project','recovery':'Pre-DDR fixed efex enters ROM 0x20 directly; no dependency on vendor Boot0 RTC handling'}
 manifest['source_spl_build_record']='build/emmc-spl-'+spl_variant+'/build-record.json'
 manifest['source_spl']=spl_provenance
 manifest['cpu_clock_acceptance']=cpu_acceptance
 manifest['cpu_clock_runtime_validated']=cpu_acceptance is not None
 if cpu_artifact_binding is not None:manifest['cpu_boot_artifact_binding']=cpu_artifact_binding
 manifest['source_spl_cold_boot_validated']=False
 manifest['variant']=variant
 manifest['peripheral_power_enabled']=variant in ('v4','v5','v6','v7','v8','v9','v10')
 manifest['display_candidate']=variant in ('v5','v6','v7','v8','v9','v10')
 manifest['rootfs']=rootfs.name
 if variant=='v7':
  manifest['credential']='User-selected credentials privately injected during offline rootfs build; no credentials in manifest'
  manifest['kernel_release']='6.18.63-t736-hdmi-v14'
  manifest['desktop']='GNOME on Xorg with Mesa llvmpipe software rendering'
 if variant in ('v8','v9','v10'):
  manifest['credential']='Existing user-selected credentials inherited from v7; no credentials in manifest'
  manifest['kernel_release']='6.18.63-t736-gpu-v15'
  manifest['desktop']='GNOME on Xorg; isolated Tina IMG EGL/GLES candidate, hardware validation required'
  manifest['vendor_executables']=['authorized DDR library and embedded training in boot chain','authorized IMG BXM 24.2.6603887 GPU userspace, firmware and shader binaries in rootfs']
  manifest['gpu_userspace_manifest']='build/gpu-userspace/manifest.json'
  manifest['gpu_runtime_validated']=False
 if variant=='v10':
  manifest['kernel_release']='6.18.63-t736-storage-v16'
  manifest['firefox_components_validated']=cpu_acceptance['firefox']
  manifest['gigabit_ethernet_accepted']=False
  manifest['hardware_video_decoding_accepted']=False
  manifest['ordinary_application_startup_latency_accepted']=False
 if variant in ('v9','v10'):
  manifest['gpu_userspace_manifest']='/opt/t736-img-ddk-24.2-x11/manifest.json'
  manifest['gpu_userspace_manifest_source']='finalized rootfs'
  manifest['gpu_userspace_manifest_sha256']=profile_record['installed_files'][manifest['gpu_userspace_manifest']]
  manifest['gpu_runtime_view_manifest_sha256']=profile_record['installed_files'].get('/opt/t736-img-ddk-24.2-x11/runtime-gles.json')
  manifest['desktop_profile']=profile_record['desktop_profile']
  manifest['desktop']='GNOME on Xorg with '+('validated IMG EGL/GLES profile; final image acceptance pending' if profile_record['desktop_profile']=='gpu' else 'Mesa llvmpipe software rendering; IMG profile remains opt-in')
  manifest['gpu_components_validated']=profile_record['desktop_profile']=='gpu'
  manifest['gpu_artifacts']=profile_record
 raw.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
if __name__=='__main__':main()
