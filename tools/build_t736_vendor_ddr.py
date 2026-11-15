#!/usr/bin/env python3
"""Build source SRAM host plus authorized pinned vendor DDR object, SDK read-only."""
import hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/vendor-ddr-v1'
SOURCES=['spl/early/entry_diag.S','spl/early/platform.c','spl/early/timer_init.c','spl/early/twi.c','spl/lpddr4/axp8191.c','spl/vendor_ddr/probe.c']
HEADERS=['spl/early/platform.h','spl/early/timer_init.h','spl/early/twi.h','spl/lpddr4/axp8191.h','spl/vendor_ddr/probe.ld','config/t736-lp4-radxa-dram-overlay.fex','tools/build_t736_vendor_ddr.py']
LIB=Path('/home/ubuntu/t736-aiot/tina/brandy/dramlib/sun60iw2p1/spl_libdram/libdram')
LIB_SHA='5361d148fa097ccc14e15a173523783d0eb1a71969bc76d57eb8e1229ddc6d90'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 OUT.mkdir(parents=True,exist_ok=True)
 assert sha(LIB)==LIB_SHA,'unapproved libdram'
 rows=re.findall(r'^(dram_\w+)\s*=\s*(\w+)',(ROOT/HEADERS[-2]).read_text(),re.M)
 assert len(rows)==32
 (OUT/'params.h').write_text('#define T736_DRAM_SEEDS '+','.join(str(int(v,0))+'u' for k,v in rows)+'\n')
 commands=[]
 def run(args):
  commands.append(args);p=subprocess.run(args,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  print(p.stdout,end='');p.check_returncode();return p.stdout
 cc='arm-linux-gnueabi-gcc'
 flags=['-mcpu=cortex-a7','-marm','-mfloat-abi=soft','-Os','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-ffunction-sections','-fdata-sections','-mno-unaligned-access','-fstack-usage','-Wall','-Wextra','-Werror','-I'+str(OUT),'-Ispl/early','-Ispl/lpddr4']
 objs=[]
 for i,s in enumerate(SOURCES):
  o=OUT/(str(i)+'.o');run([cc,*flags,'-c',s,'-o',str(o)]);objs.append(str(o))
 elf=OUT/'vendor-ddr.elf';binary=OUT/'vendor-ddr.bin'
 run([cc,*flags,'-nostdlib','-static','-no-pie','-Wl,--build-id=none,--gc-sections','-Wl,-Map='+str(OUT/'vendor-ddr.map'),'-Wl,-T,spl/vendor_ddr/probe.ld','-o',str(elf),*objs,str(LIB),'-lgcc'])
 assert not run(['arm-linux-gnueabi-nm','-u',str(elf)]).strip()
 run(['arm-linux-gnueabi-objcopy','-O','binary',str(elf),str(binary)])
 assert 0x48c00+binary.stat().st_size<=0x7d000
 (OUT/'elf-layout.txt').write_text(run(['arm-linux-gnueabi-readelf','-lW',str(elf)]))
 (OUT/'disassembly.txt').write_text(run_capture(['arm-linux-gnueabi-objdump','-d',str(elf)]))
 record={'kind':'source-host-with-authorized-vendor-ddr-library','entry':hex(0x48c00),'report':hex(0x7d000),'window_end':hex(0x7fc00),'vendor_ddr':{'path':str(LIB),'sha256':LIB_SHA},'seeds':dict((k,int(v,0)) for k,v in rows),'sources':{s:sha(ROOT/s) for s in SOURCES+HEADERS},'commands':commands,'binary_bytes':binary.stat().st_size,'binary_sha256':sha(binary),'elf_sha256':sha(elf),'compiler':run([cc,'--version']).splitlines()[0],'hardware_tested':False}
 (OUT/'build-record.json').write_text(json.dumps(record,indent=2)+'\n')
 print(json.dumps({'bytes':record['binary_bytes'],'sha256':record['binary_sha256'],'out':str(OUT)}))
def run_capture(args):return subprocess.check_output(args,text=True)
if __name__=='__main__':main()
