#!/usr/bin/env python3
"""Offline ARM32 instruction tests; UART/CP15 values are emulated, never hardware."""
from pathlib import Path
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
    UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_SPSR)

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
        UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12,
        UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC]
BASE, FAULT, UART = 0x48c00, 0x4ed00, 0x2500000


def main():
    directory = Path(sys.argv[1])
    image = (directory/'entry-diag-check.bin').read_bytes()
    symbols = {parts[2]: int(parts[0], 16) for line in (directory/'symbols.txt').read_text().splitlines()
               if len(parts := line.split()) == 3}
    instructions = [(BASE+i, struct.unpack_from('<I', image, i)[0]) for i in range(0, len(image)-3, 4)]

    def run(vector='data', uart='ok', nested=False, injected=False, normal=False):
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(0x40000, 0x10000)
        uc.mem_map(UART, 0x1000)
        uc.mem_write(BASE, image)
        report = bytes([0x5a])*2352
        uc.mem_write(0x4e000, report)
        mode = {'reset':0x13, 'undef':0x1b, 'svc':0x13, 'prefetch':0x17,
                'data':0x17, 'reserved':0x13, 'irq':0x12, 'fiq':0x11}[vector]
        if normal: mode = 0x13
        cpsr, spsr, exception_lr = 0x60000100 | mode, 0x20000153, 0x497b0
        uc.reg_write(UC_ARM_REG_CPSR, cpsr)
        uc.reg_write(UC_ARM_REG_SPSR, spsr)
        uc.reg_write(UC_ARM_REG_SP, 0xf2200)  # Intentionally outside mapped SRAM.
        uc.reg_write(UC_ARM_REG_LR, 0x41000 if normal else exception_lr)
        original = {REGS[i]: 0x13570000+i for i in range(4, 12)}
        for reg, value in original.items(): uc.reg_write(reg, value)
        cp15 = {(5,0,0):0x801, (6,0,0):0x4ffda, (5,0,1):5,
                (6,0,2):0x12345678, (1,0,0):0xc50838,
                (12,0,0):0x10000 if normal else symbols['diag_vectors']}
        output, stores, reads, stopped = [], [], {'lsr':0}, []
        if nested:
            record = [0]*32
            record[:8] = [0x54374644,1,2,1,0xdead0001,0xdead0002,0xdead0003,0xdead0004]
            uc.mem_write(FAULT, struct.pack('<32I', *record))

        def cp_hook(emu, address, size, user):
            word = struct.unpack('<I', emu.mem_read(address, 4))[0]
            key = ((word >> 16)&15, word&15, (word >> 5)&7)
            rt = (word >> 12)&15
            if word & (1 << 20):
                if injected and key == (5,0,0):
                    emu.reg_write(UC_ARM_REG_LR, 0xdeadbeec)
                    emu.reg_write(UC_ARM_REG_PC, symbols['diag_vector_data'])
                    return
                assert key in cp15, key
                emu.reg_write(REGS[rt], cp15[key])
            else:
                assert key == (12,0,0), key
                cp15[key] = emu.reg_read(REGS[rt])
            emu.reg_write(UC_ARM_REG_PC, address+4)

        def stop_hook(emu, address, size, user):
            stopped.append(address)
            emu.emu_stop()

        def read_hook(emu, access, address, size, value, user):
            assert size == 4
            if address == UART+12: val = 0x80 if uart == 'dlab' else 3
            else:
                assert address == UART+20
                reads['lsr'] += 1
                val = 0 if uart == 'thre' else 0x20 if uart == 'temt' else 0x60
            emu.mem_write(address, struct.pack('<I', val))

        def write_hook(emu, access, address, size, value, user):
            assert size == 4 and address % 4 == 0, (hex(address), size)
            if address == UART:
                assert 0 <= value <= 127
                output.append(value)
            elif FAULT <= address < FAULT+128:
                stores.append(address)
            elif normal:
                # Entry context and the tiny stub's push/pop must stay SRAM.
                assert BASE <= address < 0x50000 and not 0x4e000 <= address < 0x4ed00
            else:
                raise AssertionError(('unexpected fault-path write', hex(address), size))

        for address, word in instructions:
            if (word & 0x0f000010) == 0x0e000010 and ((word >> 8)&15) == 15:
                uc.hook_add(UC_HOOK_CODE, cp_hook, begin=address, end=address)
            if word == 0xe320f003:
                uc.hook_add(UC_HOOK_CODE, stop_hook, begin=address, end=address)
        uc.hook_add(UC_HOOK_CODE, stop_hook, begin=0x41000, end=0x41000)
        uc.hook_add(UC_HOOK_MEM_READ, read_hook, begin=UART, end=UART+0xfff)
        uc.hook_add(UC_HOOK_MEM_WRITE, write_hook)
        uc.emu_start(symbols['_start' if normal else 'diag_vector_'+vector], 0, count=12000000)
        assert stopped, 'Instruction limit reached; no bounded stop'
        assert uc.mem_read(0x4e000,2352) == report, 'Original report was modified'
        words = struct.unpack('<32I', uc.mem_read(FAULT,128))
        if normal:
            assert not any(words) and not output
            assert cp15[(12,0,0)] == 0x10000
            assert uc.reg_read(UC_ARM_REG_SP) == 0xf2200
            assert (uc.reg_read(UC_ARM_REG_CPSR)&0xff) == (cpsr&0xff)
            for reg, value in original.items(): assert uc.reg_read(reg) == value
            return
        if nested:
            assert words[:8] == tuple(record[:8]) and words[15] == 1
            assert words[17] == exception_lr and words[18] == cpsr and words[19] == 4 and not output
            return
        index = ['reset','undef','svc','prefetch','data','reserved','irq','fiq'].index(vector)
        assert words[:7] == (0x54374644,1,1 if injected else 2,index,cpsr,spsr,exception_lr)
        valid = 0x18 | (0 if index in (0,5) else 1) | (4 if index == 3 else 2 if index == 4 else 0)
        if injected:
            assert words[16] == 1 and words[15] == 1 and words[17] == 0xdeadbeec and words[19] == 4 and not output
            return
        assert words[16] == valid
        assert words[7:11] == (0x801,0x4ffda,5,0x12345678)
        assert words[13:15] == (0xc50838,symbols['diag_vectors'])
        expected = ''.join(f'{label}={value:08x}\r\n' for label,value in zip('FCSLDAIP',words[3:11])).encode()
        if uart == 'ok':
            assert bytes(output) == expected and words[11:13] == (0,1)
        elif uart == 'dlab':
            assert not output and words[11:13] == (1,0) and reads['lsr'] == 0
        elif uart == 'thre':
            assert not output and words[11:13] == (2,0) and reads['lsr'] == 1000000
        else:
            assert bytes(output) == expected and words[11:13] == (3,0)
            assert reads['lsr'] == 1000000+len(expected)
        assert words[15] == 0 and not any(words[17:])

    for vector in ['reset','undef','svc','prefetch','data','reserved','irq','fiq']: run(vector)
    for uart in ['dlab','thre','temt']: run(uart=uart)
    run(nested=True)
    run(injected=True)
    run(normal=True)
    print('PASS: 8 vector records, exact 96-byte UART output, DLAB/THRE/TEMT bounded failures, nested fault preservation, no exception-stack/original-report writes, normal context/VBAR return')


if __name__ == '__main__': main()
