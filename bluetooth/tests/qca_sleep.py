#!/usr/bin/env python3
"""Execute the real sleep bridges and caller patches (unicorn==2.1.4).

No Apple executable or personal hardware data is required. Check native ABI,
all manufacturer types, missing controller/Core instances and three ASLR slides.
"""
import importlib.util
from pathlib import Path
import re
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sleep_builder', ROOT/'tools/build-sleep-patches.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
CODE, LABELS = builder.assemble()
header = (ROOT/'compat/QcaSleep25G342.hpp').read_text()


def generated(name):
    body = re.search(r'uint8_t '+name+r'\[\] = \{(.*?)\};', header, re.S)[1]
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', body))


assert generated('HelpersReplace')[builder.HELPER_RVA-0xaa2d60:] == CODE
assert generated('HelpersFind')[:20] == generated('HelpersReplace')[:20]
assert not any(generated('HelpersFind')[20:])
REGS = [UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
        UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_R8, UC_X86_REG_R9,
        UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12, UC_X86_REG_R13,
        UC_X86_REG_R14, UC_X86_REG_R15, UC_X86_REG_RBP, UC_X86_REG_RSP]
DATA, STACK = 0x20000000, 0x30000000


class Machine:
    def __init__(self, slide):
        self.base = 0x100000000+slide
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc.mem_map(self.base, 0xc00000)
        self.uc.mem_map(DATA, 0x4000)
        self.uc.mem_map(STACK, 0x4000)
        self.uc.mem_write(self.base+builder.HELPER_RVA, CODE)
        self.stop = self.base+0xb80000
        self.uc.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, uc, address, _size, _data):
        if address in self.stops:
            self.ended = address
            uc.emu_stop()

    def reset(self, chip, core=True):
        self.uc.mem_write(DATA, b'\xa5'*0x4000)
        self.uc.mem_write(STACK, b'\x5a'*0x4000)
        for i, reg in enumerate(REGS):
            self.uc.reg_write(reg, 0x456700+i)
        self.sp = STACK+0x2000
        self.uc.reg_write(UC_X86_REG_RBP, self.sp+0x100)
        self.uc.reg_write(UC_X86_REG_RSP, self.sp)
        self.uc.reg_write(UC_X86_REG_EFLAGS, 0x247)
        self.qword(self.sp, self.stop)
        self.qword(self.base+0xb3abb0, DATA if chip is not None else 0)
        self.qword(self.base+0xb3abd0, DATA+0x1000 if core else 0)
        self.uc.mem_write(DATA+0x334, struct.pack('<I', chip or 0))
        self.stops = {self.stop, self.base+0x3224d1,
                      self.base+0x218072, self.base+0x218130}
        self.ended = None

    def qword(self, addr, value):
        self.uc.mem_write(addr, struct.pack('<Q', value))

    def regs(self):
        return {r: self.uc.reg_read(r) for r in REGS}

    def run(self, addr):
        self.uc.emu_start(self.base+addr, self.base+0xc00000, count=100)
        assert self.ended in self.stops, 'Bridge failed to reach native/return path'


cases = 0
for slide in (0, 0x100000, 0x33000000):
    m = Machine(slide)
    m.uc.mem_write(m.base+0x3224cc, generated('HidReplace'))
    for chip in (None, 0, 1, 3, 0xffffffff):
        for core in (False, True):
            for slept in (0, 1):
                for stage in range(12):
                    m.reset(chip, core)
                    m.uc.mem_write(DATA+0x1589, bytes([slept, stage]))
                    before = m.regs()
                    flags = m.uc.reg_read(UC_X86_REG_EFLAGS)
                    original_data = bytes(m.uc.mem_read(DATA, 0x4000))
                    m.run(0x3224cc)
                    blocked = chip == 0 and core and (slept or stage in (1, 3, 4))
                    assert m.ended == (m.stop if blocked else m.base+0x3224d1)
                    after = m.regs()
                    if blocked:
                        before[UC_X86_REG_RSP] += 8
                    else:
                        before[UC_X86_REG_RBP] = m.sp-8
                        before[UC_X86_REG_RSP] = m.sp-16
                    assert after == before, (slide, chip, core, slept, stage)
                    assert m.uc.reg_read(UC_X86_REG_EFLAGS) == flags
                    assert bytes(m.uc.mem_read(DATA, 0x4000)) == original_data
                    cases += 1
    for chip in (None, 0, 1, 3):
        for typ in range(256):
            for mask in (0, 1 << 21, 0xffffffffffffffff):
                snapshots = []
                for patched in (False, True):
                    m.reset(chip)
                    m.qword(m.base+0xb522c0, mask)
                    m.uc.mem_write(m.base+0x218059, generated('IdleReplace' if patched else 'IdleFind'))
                    m.uc.ctl_remove_cache(m.base+0x218000, m.base+0x219000)
                    m.uc.reg_write(UC_X86_REG_RSI, 0x123400+typ)
                    fp = m.uc.reg_read(UC_X86_REG_RBP)
                    m.uc.mem_write(fp-0x6c, struct.pack('<I', typ))
                    original_data = bytes(m.uc.mem_read(DATA, 0x4000))
                    m.run(0x218059)
                    assert bytes(m.uc.mem_read(DATA, 0x4000)) == original_data
                    snapshots.append((m.regs(), m.uc.reg_read(UC_X86_REG_EFLAGS), m.ended))
                native, candidate = snapshots
                assert candidate[0] == native[0], (slide, chip, typ, mask)
                if chip == 0 and typ == 21:
                    assert candidate[1] & 0x40, (slide, chip, typ, mask, candidate, native) # ZF
                    assert candidate[2] == m.base+0x218130
                else:
                    assert candidate == native, (slide, chip, typ, mask)
                cases += 1
print(f'PASS: {cases} sleep bridge cases; native ABI, flags, all manufacturer types, ASLR and no data mutations')
