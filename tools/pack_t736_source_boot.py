#!/usr/bin/env python3
"""Fixed board boot payload and complete Armbian raw. No legacy boot executable."""
import argparse,hashlib,json,struct,subprocess,zlib
from pathlib import Path
from pack_armbian_raw_v6 import gpt,LAYOUT,SECTORS,file_digest
ROOT=Path('/home/ubuntu/t736-mainline')
def main():
 assert Path(__file__).resolve().parents[1]==ROOT
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--variant',choices=['v3','v4','v5','v6'],default='v3')
 variant=parser.parse_args().variant
 spl_variant='v4' if variant in ('v5','v6') else variant
 out=ROOT/'output';spl=ROOT/('build/emmc-spl-'+spl_variant+'/emmc-spl.bin')
 expected={'v3':'7a3b30d3a0349e2b62d06fa07699cf1284af1e60a5704d4f1330aaf5011532db',
           'v4':'a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea'}[spl_variant]
 assert file_digest(spl)==expected
 data=bytearray(spl.read_bytes());checksum=struct.unpack_from('<I',data,12)[0];struct.pack_into('<I',data,12,0x5f0a6c39)
 assert data[4:12]==b'eGON.BT0' and struct.unpack_from('<I',data,16)[0]==len(data)
 assert sum(struct.unpack('<%dI'%(len(data)//4),data))&0xffffffff==checksum
 images=[(0x47000000,ROOT/'build/source-handoff-v1/stage64.bin','44aeb8e56f64f2ccd80a1cd286460eda6d0232f9cc1e82618866826b23298535'),(0x48000000,out/'independent-candidates/t736-bl31.bin','81f53c1d1f571c74bd907317d813a5ed56c64382479caa75edd3cec43ada6dc6'),(0x52000000,out/'independent-candidates/u-boot-source-ddr-autoboot.bin','c9b05a7d207781a730c5e12aaae2bdfaf113194f8a3ea4b1e625d330e3f4018b')]
 header=[0]*128;header[:4]=[0x36333754,0x314c5053,1,3];body=bytearray();items=[]
 for i,(a,p,h) in enumerate(images):
  assert file_digest(p)==h;b=p.read_bytes();lba=24577+len(body)//512
  header[8+i*4:12+i*4]=[lba,a,len(b),zlib.crc32(b)];body.extend(b+bytes((-len(b))%512));items.append({'path':str(p.relative_to(ROOT)),'address':hex(a),'sector':lba,'bytes':len(b),'sha256':h})
 h=bytearray(struct.pack('<128I',*header));struct.pack_into('<I',h,16,zlib.crc32(h));package=h+body
 assert 24576+len(package)//512<32800
 bootpack=out/'t736-source-boot-package-v1.bin';bootpack.write_bytes(package)
 rootfs=out/('rootfs-armbian-source-'+variant+'.ext4' if variant in ('v5','v6') else 'rootfs-armbian-source-v1.ext4');assert rootfs.stat().st_size==LAYOUT[3][2]*512
 subprocess.run(['e2fsck','-fn',str(rootfs)],check=True)
 primary,backup=gpt();regions=[('primary-gpt',0,primary),('source-spl',16,spl.read_bytes()),('source-boot-package',24576,package),('gpt-compatibility-mirror',40960,primary),('backup-gpt',SECTORS-3,backup)]
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
 manifest['variant']=variant
 manifest['peripheral_power_enabled']=variant in ('v4','v5','v6')
 manifest['display_candidate']=variant in ('v5','v6')
 manifest['rootfs']=rootfs.name
 raw.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(manifest,indent=2))
if __name__=='__main__':main()
