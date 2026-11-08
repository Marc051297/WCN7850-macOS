#!/usr/bin/env python3
"""derive-build-c7.py <build.sh> <derived script>

K5 OBSD-6 without editing build.sh: writes a copy of build.sh that
  - reads the firmware directory from ${FWDIR:-firmware} (the FwBinary.cpp check, -I, the header-newer find list
    and the CORE find, the four 'firmware' words of build.sh),
  - appends $EXTRA_DEFS to the compiler flags (the c7 build passes -DQWZ_FWSEGS_DEFAULT=1),
  - runs from $QWZ_FORK_ROOT instead of from the directory of the script (the copy lives in the output directory).
With FWDIR and EXTRA_DEFS unset the copy behaves like build.sh.
Every change names its anchor text and how often it must occur: any other count stops the derivation, so a build.sh
that changes in an incompatible way is noticed instead of silently producing a c5 kext.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()

if "FWDIR" in text:
    sys.exit("derive-build-c7: %s already supports FWDIR, so no copy is needed; run it directly:\n"
             "  FWDIR=firmware-c7 EXTRA_DEFS=-DQWZ_FWSEGS_DEFAULT=1 PRODUCT=AirportWCN7850 ./build.sh <new out dir>" % src)

DEBUG_LINE = '[ "${DEBUG:-0}" = 1 ] && COMMON+=(-DDEBUG=1 -DQWZ_DEBUG -O0)\n'

CHANGES = [
    ("cd to the tree root",
     'cd "$(dirname "$0")"\n',
     'cd "${QWZ_FORK_ROOT:?}"\n'),
    ("FwBinary.cpp check (OBSD-6: build.sh line 12)",
     '[ -f firmware/FwBinary.cpp ] ||',
     '[ -f ${FWDIR:-firmware}/FwBinary.cpp ] ||'),
    ("include path (OBSD-6: build.sh line 21)",
     '-I controller -I firmware\n',
     '-I controller -I ${FWDIR:-firmware}\n'),
    ("EXTRA_DEFS after the COMMON array and its DEBUG line (OBSD-6: 'after line 26')",
     DEBUG_LINE,
     DEBUG_LINE + '[ -n "${EXTRA_DEFS:-}" ] && COMMON+=($EXTRA_DEFS)\n'),
    ("header-newer find list (OBSD-6: build.sh line 42)",
     "itl80211 firmware -name '*.h*' -newer",
     "itl80211 ${FWDIR:-firmware} -name '*.h*' -newer"),
    ("CORE find (OBSD-6: build.sh line 90)",
     "include/HAL qwz firmware -name '*.c'",
     "include/HAL qwz ${FWDIR:-firmware} -name '*.c'"),
]

for label, old, new in CHANGES:
    n = text.count(old)
    if n != 1:
        sys.exit("derive-build-c7: anchor for '%s' found %d times (expected 1) in %s" % (label, n, src))
    text = text.replace(old, new)

with open(dst, "w") as f:
    f.write(text)
print("derive-build-c7: %s -> %s (%d changes)" % (src, dst, len(CHANGES)))
