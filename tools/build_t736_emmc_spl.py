#!/usr/bin/env python3
"""Build a source eGON eMMC SPL with the authorized pinned DDR library."""
import argparse,hashlib,json,re,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/emmc-spl-v3'
SOURCES=['spl/vendor_ddr/entry_boot.S','spl/early/platform.c','spl/early/timer_init.c','spl/early/twi.c','spl/lpddr4/axp8191.c','spl/vendor_ddr/probe.c','spl/storage/emmc.c','spl/vendor_ddr/emmc_boot.c','spl/vendor_ddr/handoff32.S']
HEADERS=['spl/early/platform.h','spl/early/timer_init.h','spl/early/twi.h','spl/lpddr4/axp8191.h','spl/storage/emmc.h','spl/vendor_ddr/boot.ld','config/t736-lp4x-fixed.fex','tools/build_t736_emmc_spl.py']
LIB=Path('/home/ubuntu/t736-aiot/tina/brandy/dramlib/sun60iw2p1/spl_libdram/libdram');LIB_SHA='5361d148fa097ccc14e15a173523783d0eb1a71969bc76d57eb8e1229ddc6d90'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 global OUT
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--variant',choices=['v3','v4','v5'],default='v3')
 args=parser.parse_args()
 OUT=ROOT/('build/emmc-spl-'+args.variant)
 sources=list(SOURCES);headers=list(HEADERS)
 if args.variant in ('v4','v5'):
  sources.append('spl/peripheral/board_power.c');headers.append('spl/peripheral/board_power.h')
 if args.variant=='v5':
  sources.append('spl/cpuclock/cpuclock.c')
  headers+=['spl/cpuclock/cpuclock.h','spl/cpuclock/test_cpuclock.c']
 OUT.mkdir(parents=True,exist_ok=True);assert sha(LIB)==LIB_SHA
 rows=re.findall(r'^(dram_\w+)\s*=\s*(\w+)',(ROOT/HEADERS[-2]).read_text(),re.M);assert len(rows)==32
 (OUT/'params.h').write_text('#define T736_DRAM_SEEDS '+','.join(str(int(v,0))+'u' for k,v in rows)+'\n')
 commands=[]
 def run(a):commands.append(a);return subprocess.check_output(a,cwd=ROOT,stderr=subprocess.STDOUT,text=True)
 cc='arm-linux-gnueabi-gcc';flags=['-mcpu=cortex-a7','-marm','-mfloat-abi=soft','-Os','-ffreestanding','-fno-builtin','-fno-pie','-fno-stack-protector','-ffunction-sections','-fdata-sections','-mno-unaligned-access','-fstack-usage','-Wall','-Wextra','-Werror','-I'+str(OUT),'-Ispl/early','-Ispl/lpddr4','-Ispl/storage'];objs=[]
 if args.variant in ('v4','v5'):flags+=['-DT736_PERIPHERAL_POWER','-Ispl/peripheral']
 if args.variant=='v5':
  flags+=['-DT736_CPUCLOCK','-Ispl/cpuclock']
  native=OUT/'test_cpuclock'
  run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Ispl/cpuclock','spl/cpuclock/cpuclock.c','spl/cpuclock/test_cpuclock.c','-o',str(native)])
  print(run([str(native)]))
 for i,s in enumerate(sources):
  o=OUT/(str(i)+'.o');extra=['-D_start=t736_enter_a64'] if s.endswith('/handoff32.S') else []
  # V5 retains the original SRAM/report/stack limits. Only the new CPU unit
  # and post-DDR eMMC transport use Thumb-2. The entry, recovery, early code,
  # host and pinned DDR library retain ARM; EABI relocations interwork calls.
  # V3/V4 compilation flags remain unchanged.
  if args.variant=='v5' and s in ('spl/cpuclock/cpuclock.c','spl/storage/emmc.c'):
   extra+=['-mthumb','-Oz']
  run([cc,*flags,*extra,'-c',s,'-o',str(o)]);objs.append(str(o))
 elf=OUT/'emmc-spl.elf';raw=OUT/'emmc-spl-unpacked.bin';packed=OUT/'emmc-spl.bin'
 try:print(run([cc,*flags,'-nostdlib','-static','-no-pie','-Wl,--build-id=none,--gc-sections','-Wl,-Map='+str(OUT/'emmc-spl.map'),'-Wl,-T,spl/vendor_ddr/boot.ld','-o',str(elf),*objs,str(LIB),'-lgcc']))
 except subprocess.CalledProcessError as e:print(e.output);raise
 assert not run(['arm-linux-gnueabi-nm','-u',str(elf)]).strip()
 run(['arm-linux-gnueabi-objcopy','-O','binary',str(elf),str(raw)])
 b=bytearray(raw.read_bytes());assert b[4:12]==b'eGON.BT0';b.extend(bytes((-len(b))%8192));assert 0x47000+len(b)<=0x7fc00
 struct.pack_into('<I',b,16,len(b));struct.pack_into('<I',b,12,0x5f0a6c39);checksum=sum(struct.unpack('<%dI'%(len(b)//4),b))&0xffffffff;struct.pack_into('<I',b,12,checksum);packed.write_bytes(b)
 (OUT/'elf-layout.txt').write_text(run(['arm-linux-gnueabi-readelf','-lW',str(elf)]));(OUT/'disassembly.txt').write_text(run(['arm-linux-gnueabi-objdump','-d',str(elf)]))
 record={'kind':'source-emmc-spl-with-authorized-ddr-library','variant':args.variant,'entry':'0x47000','report':'0x7d000','window_end':'0x7fc00','vendor_ddr_sha256':LIB_SHA,'sources':{s:sha(ROOT/s) for s in sources+headers},'commands':commands,'packed_bytes':len(b),'packed_sha256':sha(packed),'egon_checksum':hex(checksum),'elf_sha256':sha(elf),'hardware_tested':False,'cold_boot_validated':False}
 if args.variant=='v5':
  record['cpu_clock']={'target_mhz':1014,'reference_hz':26000000,'pll_n':39,'external_divider':1,'ctrl':'0xf8802700','clk':'0x03000000','pmic_configuration_writes':False,'initialization':'A76 only after DDR and board power, before handoff','hardware_tested':False,'host_failure_injection_tests':'passed'}
  record['thumb_source_units']=['spl/cpuclock/cpuclock.c','spl/storage/emmc.c']
 (OUT/'build-record.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps({k:v for k,v in record.items() if k not in ('sources','commands')}))
if __name__=='__main__':main()
