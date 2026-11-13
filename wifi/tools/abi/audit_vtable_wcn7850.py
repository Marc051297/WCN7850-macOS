#!/usr/bin/env python3
"""WCN7850: audit every virtual slot of our AWDL subclass against Apple's native table (Tahoe 25G241).

Adapted from ForbiddenItlwm tools/abi/audit_vtable.py (GPL-2.0, commit 59ff7bf): any class name and object file,
overrides of non-pure natives are reported as 'override' (same method, our implementation) instead of failures,
and the controller's awdl0 factory slot is checked in a second object when given.

Usage: python3 -I tools/abi/audit_vtable_wcn7850.py --object build/obj-awdlprobe/airport_AirportWCN7850AWDLInterface.cpp.o
         [--class-name AirportWCN7850AWDLInterface] [--controller-object build/obj-airport/airport_AirportItlwmV2.cpp.o]
"""
import argparse, json, re, struct
from pathlib import Path
root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--object', type=Path, required=True)
parser.add_argument('--class-name', default='AirportWCN7850AWDLInterface')
parser.add_argument('--controller-object', type=Path)
parser.add_argument('--controller-class', default='AirportItlwm')
parser.add_argument('--out', type=Path, default=root.parents[1] / 'build')
args = parser.parse_args()

def read_object(path):
    raw = path.read_bytes()
    sections = []
    p = 32
    n = so = st = sz = 0
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        cmd, size = struct.unpack_from('<II', raw, p)
        if cmd == 0x19:
            for i in range(struct.unpack_from('<I', raw, p + 64)[0]):
                q = p + 72 + i * 80
                addr, length, offset, align, reloff, nreloc = struct.unpack_from('<QQIIII', raw, q + 32)
                sections.append(dict(addr=addr, length=length, offset=offset, reloff=reloff, nreloc=nreloc))
        if cmd == 2:
            so, n, st, sz = struct.unpack_from('<IIII', raw, p + 8)
        p += size
    symbols = []
    for i in range(n):
        si, nt, sec, desc, value = struct.unpack_from('<IBBHQ', raw, so + i * 16)
        name = raw[st + si:raw.find(b'\0', st + si, st + sz)].decode()
        symbols.append(dict(name=name, value=value, section=sec))
    return raw, sections, symbols

def relocations_for(raw, sections, symbols, vtable_symbol):
    symbol = next(x for x in symbols if x['name'] == vtable_symbol)
    section = sections[symbol['section'] - 1]
    relocations = {}
    for i in range(section['nreloc']):
        addr, info = struct.unpack_from('<II', raw, section['reloff'] + i * 8)
        assert not addr & 0x80000000
        if (info >> 27) & 1:
            relocations[addr] = symbols[info & 0xffffff]['name']
        else:
            value = struct.unpack_from('<Q', raw, section['offset'] + addr)[0]
            target_section = info & 0xffffff
            candidates = [s['name'] for s in symbols if s['section'] == target_section and s['value'] == value]
            if candidates:
                relocations[addr] = candidates[0]
            else:
                # WCN7850: a local relocation into the middle of something (not a vtable entry); name it by the
                # nearest preceding symbol so the audit can go on, and never treat it as a method.
                below = [s for s in symbols if s['section'] == target_section and s['value'] <= value]
                near = max(below, key=lambda s: s['value']) if below else None
                relocations[addr] = (f"{near['name']}+0x{value - near['value']:x}" if near else f"local@0x{value:x}")
    return symbol, section, relocations

def tail(mangled):
    """The method name and signature after the class-name prefix of a mangled member symbol, or None."""
    m = re.match(r'__ZNK?(\d+)', mangled or '')
    if not m:
        return None
    n = int(m.group(1))
    return mangled[m.end() + n:]

cls = args.class_name
ours = f'__ZN{len(cls)}{cls}'
ours_k = f'__ZNK{len(cls)}{cls}'
raw, sections, symbols = read_object(args.object)
symbol, section, relocations = relocations_for(raw, sections, symbols, f'__ZTV{len(cls)}{cls}')
tables = json.loads((root / 'native-vtables.json').read_text())
native = tables['IO80211AWDLProtocol']['entries'][:]
while native and int(native[-1]['packed'], 16) == 0:
    native.pop()
concrete = tables['AppleBCMWLANProximityInterface']['entries']
results = []
for i, e in enumerate(native):
    assert e['symbols'], e
    expected = e['symbols'][0]
    if expected == '___cxa_pure_virtual':
        expected = concrete[i]['symbols'][0].replace('__ZN30AppleBCMWLANProximityInterface', ours)
    elif e['slot'] in (0, 8):
        expected = expected.replace('__ZN19IO80211AWDLProtocol', ours)
    elif e['slot'] == 0x38:
        expected = expected.replace('__ZNK19IO80211AWDLProtocol', ours_k)
    offset = symbol['value'] - section['addr'] + 16 + e['slot']
    actual = relocations.get(offset)
    if actual == expected:
        status = 'match'
    elif actual and actual.startswith((ours, ours_k)) and tail(actual) == tail(expected):
        status = 'override'
    elif actual and actual.startswith((ours, ours_k)) and expected.startswith((ours, ours_k)) and tail(actual) != tail(expected):
        status = 'WRONG-METHOD'
    elif actual is None:
        status = 'EMPTY'
    else:
        status = 'MISMATCH'
    results.append(dict(slot=hex(e['slot']), index=i, expected=expected, actual=actual, status=status))
by = {}
for r in results:
    by[r['status']] = by.get(r['status'], 0) + 1
vt_start = symbol['value'] - section['addr'] + 16
own_slots = sorted((o - vt_start) // 8 for o in relocations if vt_start <= o < vt_start + len(native) * 8)
report = dict(object=str(args.object), class_name=cls, native_slots=len(native), our_vtable_entries=len(own_slots),
              counts=by, problems=[r for r in results if r['status'] not in ('match', 'override')],
              overrides=[dict(slot=r['slot'], index=r['index'], method=tail(r['actual'])) for r in results if r['status'] == 'override'])
if args.controller_object:
    craw, csec, csyms = read_object(args.controller_object)
    cc = args.controller_class
    csym, csection, crel = relocations_for(craw, csec, csyms, f'__ZTV{len(cc)}{cc}')
    factory = crel.get(csym['value'] - csection['addr'] + 16 + 0xdd8)
    report['controller_factory_slot_0xdd8'] = factory
args.out.mkdir(parents=True, exist_ok=True)
(args.out / 'compiled-vtable-audit-wcn7850.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: report[k] for k in ('class_name', 'native_slots', 'our_vtable_entries', 'counts')}, indent=2))
for r in report['problems']:
    print('PROBLEM', r['slot'], r['index'], 'expected', r['expected'], 'actual', r['actual'])
print('overrides of non-pure natives:', [o['method'] for o in report['overrides']])
if 'controller_factory_slot_0xdd8' in report:
    print('controller slot 0xdd8 ->', report['controller_factory_slot_0xdd8'])
