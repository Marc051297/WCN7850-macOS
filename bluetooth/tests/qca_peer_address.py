#!/usr/bin/env python3
"""Execute the assembled 25G342 candidate under x86_64 emulation, with ASLR.

Development dependency: unicorn==2.1.4. No Apple executable is needed here.
Tests execute the actual bridge instructions, including native tail-call ABI.
"""
import importlib.util
from pathlib import Path
import random
import re
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_peer', ROOT/'tools/build-peer-address-patches.py')
build_peer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_peer)
HELPERS, DECODE, LABELS = build_peer.assemble()
generated = (ROOT/'compat/QcaPeerAddress25G342.hpp').read_text()
def generated_bytes(name):
    body=re.search(r'uint8_t '+name+r'\[\] = \{(.*?)\};',generated,re.S)[1]
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',body))
assert generated_bytes('HelpersReplace')[0x78:] == HELPERS
assert generated_bytes('DecodeReplace') == DECODE
DATA, STACK = 0x20000000, 0x30000000
REGS = [UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
        UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_R8, UC_X86_REG_R9,
        UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12, UC_X86_REG_R13,
        UC_X86_REG_R14, UC_X86_REG_R15]


class Machine:
    def __init__(self, slide=0):
        self.vm = 0x100000000 + slide
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc.mem_map(self.vm, 0xc00000)
        self.uc.mem_map(DATA, 0x20000)
        self.uc.mem_map(STACK, 0x20000)
        self.put(self.vm+build_peer.HELPER_RVA, HELPERS)
        self.put(self.vm+0x729844, DECODE)
        self.stop = self.vm+0xb80000
        self.stops = {self.stop, self.vm+0x223424, self.vm+0x706900,
                      self.vm+0x714166, self.vm+0x72989e}
        self.uc.hook_add(UC_HOOK_CODE, self.hook)
        self.chip = DATA+0x1000
        self.qword(self.vm+0xb3abb0, self.chip)
        self.fp = STACK+0x8000
        self.reset()

    def hook(self, uc, address, size, _):
        if address in self.stops:
            self.ended = address
            uc.emu_stop()

    def put(self, p, b): self.uc.mem_write(p, bytes(b))
    def qword(self, p, n): self.put(p, struct.pack('<Q', n))
    def readq(self, p): return struct.unpack('<Q', self.uc.mem_read(p, 8))[0]
    def reg(self, r): return self.uc.reg_read(r)
    def set(self, r, n): self.uc.reg_write(r, n)
    def reset(self):
        for i,r in enumerate(REGS): self.set(r, 0x456700+i)
        self.set(UC_X86_REG_RBP, self.fp)
        self.sp = self.fp-0x200
        self.set(UC_X86_REG_RSP, self.sp)
        self.qword(self.sp, self.stop)
        self.ended = None
    def run(self, name):
        start = self.vm+(0x729844 if name=='decode' else build_peer.HELPER_RVA+LABELS[name])
        self.uc.emu_start(start, self.vm+0xc00000, count=400)
        assert self.ended in self.stops


for slide in [0, 0x100000, 0x33000000]:
    m = Machine(slide)
    desc = DATA+0x2000
    original = bytes([0,1,0x42,0x12,0x34,0x56,0x78,0x9a])+bytes([0xaa])*72
    identity = bytes([0,0x10,0x20,0x30,0x40,0x50,0x60])
    m.put(desc, original)
    m.set(UC_X86_REG_RDI, desc)
    m.run('clear')
    assert m.ended == m.vm+0x223424
    assert m.uc.mem_read(desc+0x33,7) == bytes(7)
    assert m.uc.mem_read(desc+0x3a,1) == b'\xaa'

    for chip in [0, 1, 3, None]:
        for legacy in [False, True]:
            for event in [0, 1, 2, 3, 4]:
                m.reset()
                m.qword(m.vm+0xb3abb0, m.chip if chip is not None else 0)
                m.put(m.chip+0x334, struct.pack('<I', chip or 0))
                m.put(desc, bytes([event])+original[1:])
                m.put(desc+0x33, bytes(7))
                m.put(m.fp-0x48, identity)
                m.qword(m.fp+8, m.vm+(0x20c3f1 if legacy else 0x1234))
                m.set(UC_X86_REG_RBX, desc)
                m.set(UC_X86_REG_R12, desc+1)
                before = {r:m.reg(r) for r in REGS}
                m.run('keep')
                assert m.uc.mem_read(desc+1,7) == identity
                expected = b'\x81'+original[2:8] if chip==0 and legacy and event==0 else bytes(7)
                assert m.uc.mem_read(desc+0x33,7) == expected, (slide,chip,legacy,event,bytes(m.uc.mem_read(desc+0x33,7)).hex(),expected.hex())
                assert m.uc.mem_read(desc+0x3a,1) == b'\xaa'
                for r,v in before.items():
                    if r not in [UC_X86_REG_RAX,UC_X86_REG_RCX]: assert m.reg(r)==v

    # Standard Extended Advertising Report carrying a legacy ADV_IND. Native
    # 25G342 parser encodes event 0xb0, raw type 0x13, non-legacy byte zero.
    # Real directed-address storage (+0x24) and the flag at +0x3a stay intact.
    for event in [0,0xb0,0xb1,0xb3,0xb4]:
        for raw in [0,0x12,0x13,0x15,0x1b]:
            for nonlegacy in [0,1]:
                m.reset();m.put(desc,original)
                m.put(desc,bytes([event]));m.put(desc+0x1a,struct.pack('<H',raw))
                m.put(desc+0x2b,bytes([nonlegacy]))
                before=bytes(m.uc.mem_read(desc,len(original)))
                m.set(UC_X86_REG_RDI,desc)
                m.run('extended')
                eligible=event==0xb0 and raw==0x13 and nonlegacy==0
                expected=bytearray(before)
                if eligible:expected[0x33:0x3a]=bytes(7)
                assert bytes(m.uc.mem_read(desc,len(original)))==bytes(expected)
                assert m.ended==m.vm+0x223424 and m.reg(UC_X86_REG_RDI)==desc
                for chip in [0,1,3,None]:
                    m.reset();m.put(desc,expected)
                    m.qword(m.vm+0xb3abb0,m.chip if chip is not None else 0)
                    m.put(m.chip+0x334,struct.pack('<I',chip or 0))
                    m.put(m.fp-0x48,identity);m.qword(m.fp+8,m.vm+0x20dc6f)
                    m.set(UC_X86_REG_RBX,desc);m.set(UC_X86_REG_R12,desc+1)
                    m.run('keep')
                    want=b'\x81'+original[2:8] if eligible and chip==0 else bytes(expected[0x33:0x3a])
                    assert bytes(m.uc.mem_read(desc+0x33,7))==want,(slide,event,raw,nonlegacy,chip)
                    assert bytes(m.uc.mem_read(desc+1,7))==identity
                    assert bytes(m.uc.mem_read(desc+0x24,7))==before[0x24:0x2b]

    rng=random.Random(0x7850)
    packet = DATA+0x4000
    for _ in range(500):
        m.reset()
        typ=rng.choice([0,1,2,0x81,0xff]); event=rng.choice([0,1,2,0xb0,0xb1,0xb3]); extended=rng.randrange(2)
        address=bytes(rng.randrange(256) for _ in range(6))
        typed=bytes([typ])+address
        m.put(desc+0x33,typed)
        m.qword(packet+0x1768,desc+0x33)
        m.put(packet,bytes([event]));m.put(packet+0x1756,bytes([extended]))
        m.set(UC_X86_REG_R14,packet)
        cl,dl=m.reg(UC_X86_REG_RCX),m.reg(UC_X86_REG_RDX)
        m.run('decode')
        valid=typ in [0,1] or (typ==0x81 and event in [0,0xb0] and extended==0 and address[0]&0xc0==0x40)
        assert m.reg(UC_X86_REG_RAX)==(int.from_bytes(typed,'big') if valid else 0)
        assert m.reg(UC_X86_REG_RCX)==cl and m.reg(UC_X86_REG_RDX)==dl
        assert m.reg(UC_X86_REG_RSP)==m.sp

    device, advertisement, wrapper, parent = DATA+0x8000,DATA+0x9000,DATA+0xa000,STACK+0xa000
    tagged=int.from_bytes(b'\x81\x42\x12\x34\x56\x78\x9a','big')
    for chip in [0,3,None]:
        for native_caller in [False,True]:
            for present in [False,True]:
                for saved_identity in [0,0x123456]:
                    m.reset()
                    m.qword(m.vm+0xb3abb0,m.chip if chip is not None else 0)
                    m.put(m.chip+0x334,struct.pack('<I',chip or 0))
                    m.qword(m.fp,parent);m.qword(parent-0x480,wrapper);m.qword(wrapper+8,advertisement)
                    m.qword(m.fp+8,m.vm+(0x73b88c if native_caller else 0x1234))
                    m.qword(advertisement+0xd0,tagged)
                    m.qword(device+0x10,0xabcdef);m.qword(device+0x18,saved_identity)
                    m.set(UC_X86_REG_RAX,device if present else 0);m.set(UC_X86_REG_R14,1)
                    before={r:m.reg(r) for r in REGS}
                    m.run('device')
                    assert m.readq(advertisement+0xd0)==(0 if native_caller else tagged)
                    changed=chip==0 and native_caller and present
                    assert m.readq(device+0x10)==(tagged & ~(1<<55) if changed else 0xabcdef)
                    assert m.readq(device+0x18)==(saved_identity or 0xabcdef if changed else saved_identity)
                    for r,v in before.items(): assert m.reg(r)==v
                    assert m.reg(UC_X86_REG_RSP)==m.sp+8
                    if present: assert m.uc.mem_read(device+0x61,1)==b'\x01'

    for chip in [0,1,3,None]:
        for name in ['get','list']:
            m.reset();m.qword(m.vm+0xb3abb0,m.chip if chip is not None else 0)
            m.put(m.chip+0x334,struct.pack('<I',chip or 0))
            args=(m.reg(UC_X86_REG_RDI),m.reg(UC_X86_REG_RSI))
            m.run(name)
            assert args==(m.reg(UC_X86_REG_RDI),m.reg(UC_X86_REG_RSI))
            if name=='get':
                assert m.ended==m.vm+0x706900 and m.reg(UC_X86_REG_RDX)==int(chip==0)
            elif chip==0: assert m.ended==m.stop and m.reg(UC_X86_REG_RAX)==0
            else: assert m.ended==m.vm+0x714166

print('PASS: real x86_64 peer bridges, ASLR, identity retention, caller/controller scoping, null devices and native ABI')
