#!/usr/bin/env python3
"""Compile the real native/legacy TX parameter picker; no radio is changed."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'qwz/qwz_mon.c').read_text()
start = source.index('#define QWZ_MON_TXP_MC0')
end = source.index('{', source.index('qwz_mon_fmt_pick(')) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]
header = (root/'qwz/qwz_mon.h').read_text()
flag = re.search(r'^#define QWZ_MONTX_F_NO_ACK\s+\S+', header, re.M).group()
code = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
static int qwz_monfmt=0;
''' + flag + '\n' + body + r'''
int main() {
    uint32_t d0=0,d1=0;unsigned retry=0,rate=0;
    // Off explicitly stays off even if an injection call has left context
    // on its stack while waiting for credits. Outputs remain untouched.
    qwz_monfmt=2;qwz_mon_fmt_user=1;qwz_mon_fmt_rate=108;
    for(int mode=0;mode<=5;++mode) {
        d0=d1=77;retry=rate=88;
        assert(!qwz_mon_fmt_pick(2,1200,&d0,&d1,&retry,&rate,mode));
        assert(d0==77 && d1==77 && retry==88 && rate==88);
        assert(!qwz_mon_fmt_pick(1,1400,&d0,&d1,&retry,&rate,mode));
        assert(!qwz_mon_fmt_pick(3,1400,&d0,&d1,&retry,&rate,mode));
        assert(!qwz_mon_fmt_pick(2,1199,&d0,&d1,&retry,&rate,mode));
    }
    // Legacy injector behavior and header-supplied rate/NO_ACK remain intact.
    qwz_mon_fmt_flags=QWZ_MONTX_F_NO_ACK;
    assert(qwz_mon_fmt_pick(2,1400,&d0,&d1,&retry,&rate));
    assert(retry==1 && rate==108 && ((d0>>8)&0xfff)==(1U<<11));
    qwz_monfmt=1;
    assert(!qwz_mon_fmt_pick(2,1400,&d0,&d1,&retry,&rate));
    assert(qwz_mon_fmt_pick(1,149,&d0,&d1,&retry,&rate));
    assert(retry==2 && rate==24 && !(d1&(1U<<20)));
    qwz_mon_fmt_user=0;
    assert(!qwz_mon_fmt_pick(1,149,&d0,&d1,&retry,&rate));
    puts("native bulk TX profiles blocked, explicit context and legacy regression: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-native-format-') as directory:
    src=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    src.write_text(code)
    subprocess.run(['xcrun','clang++','-std=c++14','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
