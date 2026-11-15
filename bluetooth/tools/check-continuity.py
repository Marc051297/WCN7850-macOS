#!/usr/bin/env python3
"""Read-only check of every required x86_64 bluetoothd patch site."""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def check(path, build):
    source = (ROOT / 'compat/QcaContinuityPatches.hpp').read_text()
    source = re.sub(r'//[^\n]*|/\*.*?\*/', '', source, flags=re.S)
    arrays = {name: bytes(int(n, 16) for n in re.findall(r'0x[0-9a-fA-F]+', body))
              for name, body in re.findall(r'uint8_t\s+(\w+)\[\]\s*=\s*\{(.*?)\}', source, re.S)}
    data = path.read_bytes()
    results = []
    suffix = '' if build == '25G241' else '2673'
    sites = [(name, suffix, index + (0 if build == '25G241' else 4))
             for index, name in enumerate(('advertising', 'randomAddress', 'extendedConnection', 'scanning'))]
    # Both signed 25G332 and 25G342 binaries contain TWO connection setter sites.
    # The upstream name '2672' describes its origin, not an exclusive build match.
    if build != '25G241': sites.append(('extendedConnection', '2672', 8))
    mask = 0
    for name, suffix, bit in sites:
        find, replace = arrays[name + 'Find' + suffix], arrays[name + 'Replace' + suffix]
        count = data.count(find)
        offset = data.find(find)
        within_page = offset >= 0 and offset // 4096 == (offset + len(find) - 1) // 4096
        ok = count == 1 and len(find) == len(replace) and within_page
        results.append(dict(site=name + suffix, patch_bit=bit, unique=count == 1, within_page=within_page, ok=ok))
        if ok: mask |= 1 << bit
    return dict(build=build, sha256=hashlib.sha256(data).hexdigest(), sites=results,
                expected_patch_mask=mask, ok=all(r['ok'] for r in results))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='Pristine bluetoothd, or run before enabling -qcaadv')
    parser.add_argument('--build', required=True, choices=('25G241', '25G332', '25G342'))
    args = parser.parse_args()
    result = check(args.binary, args.build)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['ok'] else 1)
