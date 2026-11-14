// SPDX-License-Identifier: MIT
// Decode only the audited XNU x86_64 VNOP_VERIFY instruction pair. Unknown
// register allocation/layout returns zero, leaving the optional feature off.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace QcaBluetooth {
inline uint32_t verifierVectorOffset(const uint8_t *text, size_t length,
                                     uint64_t address, uint64_t descriptor) {
    if (!text || length < 14) return 0;
    for (size_t i = 0; i <= length - 14; ++i) {
        // movq disp32(%r15), %rcx; movslq desc(%rip), %rdx
        if (text[i] != 0x49 || text[i+1] != 0x8b || text[i+2] != 0x8f ||
            text[i+7] != 0x48 || text[i+8] != 0x63 || text[i+9] != 0x15) continue;
        int32_t displacement, relative;
        memcpy(&displacement, text+i+3, 4);
        memcpy(&relative, text+i+10, 4);
        const uint64_t target = address + i + 14 + uint64_t(int64_t(relative));
        if (target != descriptor || displacement <= 0 || displacement >= 512 ||
            displacement % sizeof(void *)) continue;
        return uint32_t(displacement);
    }
    return 0;
}
}
