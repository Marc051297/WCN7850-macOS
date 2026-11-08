#!/bin/bash
# build-c7.sh <out dir>  ->  <out dir>/AirportWCN7850.kext with the c7-00108 firmware and QWZ_FWSEGS_DEFAULT=1
# K5 OBSD-6. build.sh is not edited: derive-build-c7.py writes a copy of it into <out dir> with the OBSD-6 changes
# (FWDIR, EXTRA_DEFS), and that copy builds the tree from the tree root against firmware-c7/ instead of firmware/.
# The output directory must be a new one: build.sh links every object in <out dir>/obj-airport, so a directory
# shared with a c5 build would link two qwz_fw_table symbols. The tree's own build/ is refused.
# Run firmware-c7/fetch-firmware-c7.sh first (it writes dl/, FwBinary.cpp and qwz_fw.h here).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
[ $# -eq 1 ] || { echo "usage: $0 <out dir>"; exit 2; }
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
case "$OUT/" in
  "$ROOT/build/"*|"$ROOT/build-debug/"*|"$ROOT/firmware-c7/"*|"$ROOT/firmware/"*)
    echo "STOP: $OUT is a c5 output or firmware directory; give the c7 build its own"; exit 2 ;;
esac
for f in FwBinary.cpp qwz_fw.h SHA256SUMS; do
  [ -f "$HERE/$f" ] || { echo "STOP: $HERE/$f missing, run firmware-c7/fetch-firmware-c7.sh first"; exit 1; }
done
(cd "$HERE/dl" && shasum -a 256 -c ../SHA256SUMS >/dev/null) || { echo "STOP: firmware-c7/dl does not match firmware-c7/SHA256SUMS"; exit 1; }
for f in "$HERE"/dl/qwz-*; do
  [ "$HERE/FwBinary.cpp" -nt "$f" ] || { echo "STOP: $f is newer than firmware-c7/FwBinary.cpp, regenerate it"; exit 1; }
done
python3 -I "$HERE/derive-build-c7.py" "$ROOT/build.sh" "$OUT/build-derived.sh"
QWZ_FORK_ROOT=$ROOT FWDIR=firmware-c7 EXTRA_DEFS=-DQWZ_FWSEGS_DEFAULT=1 PRODUCT=AirportWCN7850 bash "$OUT/build-derived.sh" "$OUT"
