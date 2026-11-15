#!/usr/bin/env python3
"""Offline link/emulation of optional exception entry. Stub is NOT a board payload."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'build/entry-diag-check'
INPUTS = ['spl/early/entry_diag.S','spl/early/entry_diag_check.S','spl/early/entry_diag_check.ld',
          'tools/build_t736_entry_diag.py','tools/test_t736_entry_diag.py']


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    commands=[]
    def run(args):
        args=list(map(str,args));commands.append(args)
        p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        if p.returncode: print(p.stdout);p.check_returncode()
        return p.stdout
    flags=['-mcpu=cortex-a55','-marm','-mfloat-abi=soft','-mno-unaligned-access','-g','-ffreestanding','-fno-pic']
    objects=[]
    for name in INPUTS[:2]:
        obj=OUT/(Path(name).stem+'.o')
        run(['arm-linux-gnueabi-gcc',*flags,'-c',ROOT/name,'-o',obj]);objects.append(obj)
    elf,raw=OUT/'entry-diag-check.elf',OUT/'entry-diag-check.bin'
    run(['arm-linux-gnueabi-gcc',*flags,'-nostdlib','-no-pie','-Wl,--build-id=none',
         '-Wl,-T,'+str(ROOT/INPUTS[2]),'-Wl,-Map,'+str(OUT/'entry-diag-check.map'),*objects,'-o',elf])
    undefined=run(['arm-linux-gnueabi-nm','-u',elf]);assert not undefined.strip(),undefined
    symbols=run(['arm-linux-gnueabi-nm','-n',elf]);(OUT/'symbols.txt').write_text(symbols)
    header=run(['arm-linux-gnueabi-readelf','-h',elf]);segments=run(['arm-linux-gnueabi-readelf','-lW',elf])
    attrs=run(['arm-linux-gnueabi-readelf','-A',elf]);assert 'Tag_CPU_unaligned_access: v6' not in attrs
    assert re.search(r'Entry point address:\s+0x48c00',header)
    for fields in re.findall(r'LOAD\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)',segments):
        va,pa,fs,ms=map(lambda v:int(v,16),fields);assert va==pa and 0x48c00<=va and va+ms<=0x50000 and fs<=ms
    run(['arm-linux-gnueabi-objcopy','-O','binary',elf,raw]);assert raw.stat().st_size<0x5400
    disasm=run(['arm-linux-gnueabi-objdump','-d',elf]);(OUT/'disassembly.txt').write_text(disasm)
    (OUT/'readelf.txt').write_text(header+segments+attrs)
    for line in disasm.splitlines():
        match=re.search(r'\b(?:ldr|str)(?:eq|ne|lo|hs|hi|ls)?\s+.*\[[^\]]+, #(-?\d+)\]',line)
        if match: assert int(match[1])%4==0,line
    output=run([sys.executable,ROOT/'tools/test_t736_entry_diag.py',OUT]);print(output,end='')
    record={'kind':'offline-exception-entry-validation-stub-not-for-hardware','hardware_tested':False,
            'entry':'0x48c00','fault_record':'0x4ed00','fault_record_bytes':128,
            'max_probe_report_end':'0x4e930','stack_bottom':'0x4f000','raw_bytes':raw.stat().st_size,
            'undefined_symbols':[],'unaligned_access_disabled':True,'emulated_instruction_tests':True,
            'test_output':output,'commands':commands,
            'input_sha256':{n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in INPUTS},
            'output_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [elf,raw]}}
    (OUT/'build-record.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'raw_bytes':record['raw_bytes'],'output_sha256':record['output_sha256']},indent=2))


if __name__ == '__main__':main()
