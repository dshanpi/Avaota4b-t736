#!/usr/bin/env python3
import hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/source-handoff-v1'
SOURCES=['spl/vendor_ddr/handoff32.S','spl/vendor_ddr/stage64.S','spl/vendor_ddr/stage64.c']
HEADERS=['tools/build_t736_source_handoff.py']
def main():
 OUT.mkdir(parents=True,exist_ok=True);commands=[]
 for name,arch,base,files,flags in [('handoff32','arm-linux-gnueabi',0x48c00,SOURCES[:1],['-march=armv8-a','-marm','-mfloat-abi=soft']),('stage64','aarch64-linux-gnu',0x47000000,SOURCES[1:],['-march=armv8-a','-mgeneral-regs-only'])]:
  ld=OUT/(name+'.ld');ld.write_text('ENTRY(_start)\nSECTIONS { . = '+hex(base)+'; .text : { *(.text.entry) *(.text*) *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } /DISCARD/ : { *(.comment .note* .eh_frame* .ARM.exidx*) } }\n')
  elf=OUT/(name+'.elf');binary=OUT/(name+'.bin')
  cmd=[arch+'-gcc',*flags,'-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pie','-nostdlib','-static','-no-pie','-Wall','-Wextra','-Werror','-Wl,--build-id=none','-Wl,-T,'+str(ld),*files,'-o',str(elf)]
  commands.append(cmd);subprocess.run(cmd,cwd=ROOT,check=True);subprocess.run([arch+'-objcopy','-O','binary',str(elf),str(binary)],check=True)
  (OUT/(name+'.dis')).write_text(subprocess.check_output([arch+'-objdump','-d',str(elf)],text=True))
  assert not subprocess.check_output([arch+'-nm','-u',str(elf)],text=True).strip()
  assert binary.stat().st_size<8192
 record={'sources':{s:hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in SOURCES+HEADERS},'commands':commands,'outputs':{p.name:{'size':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in OUT.glob('*.bin')}}
 (OUT/'build-record.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record['outputs']))
if __name__=='__main__':main()
