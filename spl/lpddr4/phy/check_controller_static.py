#!/usr/bin/env python3
"""Offline tests and A32 object audit only; never accesses board hardware."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
OUT=ROOT/'build/controller-static'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    commands=[]
    def run(args):
        args=list(map(str,args));commands.append(args)
        p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        if p.returncode:print(p.stdout);p.check_returncode()
        return p.stdout
    native=OUT/'test_controller_static'
    run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wconversion','-Wshadow',
         '-fsanitize=address,undefined','-fno-omit-frame-pointer',HERE/'test_controller_static.c','-o',native])
    result=run([native]);print(result,end='')
    obj=OUT/'controller_static.o'
    flags=['-mcpu=cortex-a55','-marm','-mfloat-abi=soft','-mno-unaligned-access','-O2','-g',
           '-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-fstack-usage',
           '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections',
           '-Wall','-Wextra','-Werror','-Wconversion','-Wshadow']
    run(['arm-linux-gnueabi-gcc',*flags,'-c',HERE/'controller_static.c','-o',obj])
    undefined=run(['arm-linux-gnueabi-nm','-u',obj])
    if undefined.strip():raise RuntimeError('Undefined object dependency: '+undefined)
    attrs=run(['arm-linux-gnueabi-readelf','-A',obj])
    if 'Tag_CPU_unaligned_access: v6' in attrs:raise RuntimeError('Unaligned access enabled')
    disasm=run(['arm-linux-gnueabi-objdump','-d',obj])
    for line in disasm.splitlines():
        match=re.search(r'\b(?:ldr|str)(?:eq|ne|gt|lt|ge|le|cc|cs|hi|ls)?\s+\w+,\s*\[\w+, #(-?\d+)\]',line)
        if match and int(match[1])%4:raise RuntimeError('Review unaligned word offset: '+line)
    (OUT/'readelf.txt').write_text(attrs)
    (OUT/'disassembly.txt').write_text(disasm)
    stack=0
    for p in OUT.glob('*.su'):
        for line in p.read_text().splitlines():
            _,size,allocation=line.rsplit('\t',2)
            if allocation!='static':raise RuntimeError('Dynamic stack: '+line)
            stack+=int(size)
    if stack>=4096:raise RuntimeError('Conservative stack budget exceeded')
    inputs=[HERE/'controller_static.c',HERE/'controller_static.h',HERE/'host.h',HERE/'test_controller_static.c',Path(__file__).resolve()]
    record={'kind':'controller-static-component-offline-not-board-payload','hardware_tested':False,
            'counter_clock_assumption_mhz':450,'ck_mhz':1800,'expected_writes':22,
            'expected_reads_immediate_swstat':142,'native_asan_ubsan_pass':True,'test_output':result,
            'arm_undefined_symbols':[],'unaligned_access_disabled':True,'stack_frame_sum_bytes':stack,
            'commands':commands,'input_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
            'object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest()}
    (OUT/'build-record.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'stack_frame_sum_bytes':stack,'object_sha256':record['object_sha256']},indent=2))

if __name__=='__main__':main()
