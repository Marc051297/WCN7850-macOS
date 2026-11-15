// SPDX-License-Identifier: MIT
#include "QcaValidatedPages.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
using Ledger = QcaBluetooth::ValidatedPages<3>;

int main() {
    Ledger pages;
    std::array<uint8_t, 16384> original, modified, copy;
    std::mt19937 random(0x7850);
    for (auto &b : original) b = uint8_t(random());
    modified = original;
    for (size_t pos : {size_t(33), size_t(4094), size_t(4095), size_t(4096),
                       size_t(8195), size_t(8200), size_t(8201)}) modified[pos] ^= 0xa5;
    assert(!pages.bind(0));
    assert(pages.bind(1) && pages.bind(1) && !pages.bind(2));
    assert(pages.reserve(1, 1) == -1 && pages.reserve(2, 0) == -1);
    for (size_t i = 0; i < 3; ++i)
        assert(pages.record(1, i * 4096, original.data() + i * 4096,
                            modified.data() + i * 4096));
    assert(pages.reserve(1, 12288) == -1);
    assert(!pages.record(1, 12288, original.data(), modified.data()));
    copy = modified;
    assert(pages.restore(2, 0, copy.data(), copy.size()) == 0 && copy == modified);
    assert(pages.restore(1, 0, copy.data(), copy.size()) == 7 && copy == original);
    assert(pages.restore(1, 0, copy.data(), copy.size()) == 0);
    assert(!pages.record(1, 0, modified.data(), modified.data()));
    assert(pages.originalMatches(1, 0, original.data()));
    assert(!pages.originalMatches(1, 0, modified.data()));
    // A tampered changed run is not repaired; an unrelated corruption survives.
    copy = modified;
    copy[8200] ^= 1;
    copy[6000] ^= 1;
    pages.restore(1, 0, copy.data(), copy.size());
    assert(copy[8200] != original[8200] && copy[8201] == modified[8201]);
    assert(copy[6000] != original[6000]);
    // Arbitrarily aligned verifier blocks, including partial pages and patches.
    for (int n = 0; n < 5000; ++n) {
        size_t first = random() % original.size();
        size_t length = random() % (original.size() - first) + 1;
        copy = modified;
        pages.restore(1, first, copy.data() + first, length);
        assert(!memcmp(copy.data() + first, original.data() + first, length));
        assert(!memcmp(copy.data(), modified.data(), first));
        assert(!memcmp(copy.data() + first + length, modified.data() + first + length,
                       copy.size() - first - length));
    }
    copy = modified;
    assert(!pages.restore(1, UINT64_MAX - 4, copy.data(), 8));
    assert(!pages.restore(1, 0, nullptr, 12));
    assert(copy == modified);
    Ledger reserved;
    assert(reserved.bind(3));
    assert(reserved.hold(3, 8192));
    assert(reserved.record(3, 0, original.data(), modified.data()));
    assert(reserved.record(3, 4096, original.data()+4096, modified.data()+4096));
    assert(reserved.reserve(3, 12288) == -1);
    assert(reserved.record(3, 8192, original.data()+8192, modified.data()+8192));
    copy = modified;
    assert(reserved.restore(3, 0, copy.data(), copy.size()) == 7 && copy == original);
    puts("PASS: validated-page originals, vnode isolation, repeated validation, bounds and tamper preservation");
}
