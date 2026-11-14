// SPDX-License-Identifier: MIT
#include "QcaVerifyLayout.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
    uint8_t code[] = {0x90,0x49,0x8b,0x8f,0xd0,0,0,0,0x48,0x63,0x15,0x20,0,0,0,0xc3};
    using QcaBluetooth::verifierVectorOffset;
    assert(verifierVectorOffset(code,sizeof(code),0x1000,0x102f)==0xd0);
    for (size_t n=0;n<15;++n) assert(!verifierVectorOffset(code,n,0x1000,0x102f));
    assert(!verifierVectorOffset(nullptr,100,0x1000,0x102f));
    assert(!verifierVectorOffset(code,sizeof(code),0x1000,0x1030));
    code[4]=0xd1;
    assert(!verifierVectorOffset(code,sizeof(code),0x1000,0x102f));
    code[4]=0xd0; code[3]=0x87;
    assert(!verifierVectorOffset(code,sizeof(code),0x1000,0x102f));
    code[3]=0x8f;
    int32_t relative=-0x100;
    memcpy(code+11,&relative,4);
    assert(verifierVectorOffset(code,sizeof(code),0x1000,0xf0f)==0xd0);
    if (argc == 4) {
        uint8_t native[256];
        FILE *file=fopen(argv[1],"rb");
        assert(file && fread(native,1,sizeof(native),file)==sizeof(native));
        fclose(file);
        assert(verifierVectorOffset(native,sizeof(native),strtoull(argv[2],nullptr,0),
                                     strtoull(argv[3],nullptr,0))==0xd0);
        puts("PASS: captured matching-kernel VNOP_VERIFY decodes vnode operations at 0xd0");
    }
    puts("PASS: exact verifier instruction layout, descriptor relocation and unknown-layout rejection");
}
