#!/usr/bin/env python3
"""Generate editor commands from build.sh's actual flags/source selection.

Only its COMMON declaration and product-selection tail are evaluated. A capture
function replaces build_kext, so no compiler, link, firmware fetch or SDK clone
runs. Regenerate after changing build.sh, target selection or SDK location.
"""
import argparse
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=root/'compile_commands.json')
args = parser.parse_args()
source = (root/'build.sh').read_text()
common = source[source.index('COMMON=('):source.index('# build_kext <name>')]
products = source[source.index('CORE=$(find'):]
capture = r'''
set -e
cd "$1"
ROOT=$PWD
OUT=$ROOT/build-editor
MKSDK=$ROOT/deps/MacKernelSDK
VERSION=$(cat VERSION)
PRODUCT=AirportWCN7850
'''+common+r'''
build_kext() {
    local NAME=$1 PLIST=$2 OBJ=$OUT/obj-$3 SRCS=$4
    shift 4
    local FLAGS=("${COMMON[@]}" "$@")
    local s
    for s in $SRCS; do
        printf '%s\0' "$s" clang++ "${FLAGS[@]}" -c "$s" -o "$OBJ/${s//\//_}.o"
        printf '\0'
    done
}
'''+products
result = subprocess.run(['bash','-s','--',str(root)],input=capture.encode(),capture_output=True,check=True)
compiler = subprocess.check_output(['xcrun','--find','clang++'],text=True).strip()
commands=[]
for record in result.stdout.split(b'\0\0'):
    if not record: continue
    fields=[s.decode() for s in record.rstrip(b'\0').split(b'\0')]
    filename,_,*flags=fields
    commands.append(dict(directory=str(root),file=str(root/filename),arguments=[compiler,*flags]))
assert commands and any(Path(c['file']).name=='AirportItlwmV2.cpp' for c in commands)
args.output.write_text(json.dumps(commands,indent=2)+'\n')
print(f'{args.output}: {len(commands)} translation units')
