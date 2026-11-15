#!/usr/bin/env python3
"""Execute the nativefmt sysctl handler; unsafe modes must not be accepted."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'qwz/qwz_sysctl.cpp').read_text()
start = source.index('static int\nqwz_sysctl_nativefmt(')
end = source.index('{', start)+1
depth = 1
while depth:
    depth += (source[end] == '{')-(source[end] == '}')
    end += 1
body = source[start:end]
code = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
static int qwz_nativefmt=0,observed=-1;
struct sysctl_oid {};
struct sysctl_req {uintptr_t newptr=0;int value=0,error=0;};
static int sysctl_handle_int(sysctl_oid *,int *v,int,sysctl_req *r) {
    observed=*v;if(r->newptr)*v=r->value;return r->error;
}
''' + body + r'''
int main() {
    sysctl_req r;
    assert(!qwz_sysctl_nativefmt(nullptr,nullptr,0,&r) && observed==0);
    r.newptr=1;
    for(int v:{-1,1,2,3,100}) {
        r.value=v;assert(qwz_sysctl_nativefmt(nullptr,nullptr,0,&r)==ENOTSUP);
        assert(qwz_nativefmt==0);
    }
    r.value=0;qwz_nativefmt=2;
    assert(!qwz_sysctl_nativefmt(nullptr,nullptr,0,&r) && qwz_nativefmt==0);
    r.error=EFAULT;assert(qwz_sysctl_nativefmt(nullptr,nullptr,0,&r)==EFAULT);
    puts("native rate sysctl accepts only restore/read zero: PASS");
}
'''
code=code.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='qwz-format-block-') as directory:
    src=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    src.write_text(code)
    subprocess.run(['xcrun','clang++','-std=c++14','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
