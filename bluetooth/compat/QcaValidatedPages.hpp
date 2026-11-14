// SPDX-License-Identifier: MIT
// Remember only changes made to already validated code pages. A verifier gets
// the original bytes for our changes; all unrelated bytes remain verifiable.
// The caller supplies synchronization and owns the lifetime of the file token.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace QcaBluetooth {
template <size_t Slots, size_t PageBytes = 4096>
class ValidatedPages {
    struct Entry {
        uint64_t offset;
        bool used;
        bool recorded;
        uint8_t original[PageBytes];
        uint8_t modified[PageBytes];
    };
    Entry entries[Slots] {};
    uintptr_t file {};

public:
    // A different vnode cannot inherit saved bytes from an earlier executable.
    bool bind(uintptr_t token) {
        if (!token || (file && file != token)) return false;
        file = token;
        return true;
    }
    bool matches(uintptr_t token) const { return file && file == token; }
    int reserve(uintptr_t token, uint64_t offset) const {
        if (!matches(token) || offset % PageBytes ||
            offset > UINT64_MAX - PageBytes) return -1;
        int free = -1;
        for (size_t i = 0; i < Slots; ++i) {
            if (entries[i].used && entries[i].offset == offset) return int(i);
            if (!entries[i].used && free < 0) free = int(i);
        }
        return free;
    }
    bool record(uintptr_t token, uint64_t offset, const void *before,
                const void *after) {
        const int slot = reserve(token, offset);
        if (slot < 0 || !before || !after) return false;
        Entry &e = entries[slot];
        // Revalidation is valid only against the same original page. Never
        // replace the trusted original with a partially restored/tampered copy.
        if (e.recorded && memcmp(e.original, before, PageBytes)) return false;
        if (!e.recorded) memcpy(e.original, before, PageBytes);
        memcpy(e.modified, after, PageBytes);
        e.offset = offset;
        e.used = true;
        e.recorded = true;
        return true;
    }
    // Reserve helper-page storage before any caller is redirected there. The
    // helper's original bytes are recorded when that page first validates.
    bool hold(uintptr_t token, uint64_t offset) {
        const int slot = reserve(token, offset);
        if (slot < 0) return false;
        entries[slot].offset = offset;
        entries[slot].used = true;
        return true;
    }
    bool originalMatches(uintptr_t token, uint64_t offset, const void *page) const {
        if (!matches(token) || !page) return false;
        for (const auto &e : entries)
            if (e.recorded && e.offset == offset)
                return memcmp(e.original, page, PageBytes) == 0;
        return true; // first validated observation
    }
    size_t restore(uintptr_t token, uint64_t offset, void *buffer, size_t size) const {
        if (!matches(token) || !buffer || !size || size > UINT64_MAX - offset)
            return 0;
        auto *bytes = static_cast<uint8_t *>(buffer);
        size_t restored = 0;
        for (const auto &e : entries) {
            if (!e.recorded || e.offset >= offset + size || e.offset + PageBytes <= offset)
                continue;
            const size_t lo = e.offset < offset ? size_t(offset - e.offset) : 0;
            const size_t hi = e.offset + PageBytes > offset + size ?
                size_t(offset + size - e.offset) : PageBytes;
            for (size_t i = lo; i < hi; ) {
                if (e.original[i] == e.modified[i]) { ++i; continue; }
                size_t end = i + 1;
                while (end < hi && e.original[end] != e.modified[end]) ++end;
                uint8_t *dst = bytes + size_t(e.offset + i - offset);
                // An unexpected byte leaves the whole changed run untouched.
                // The real verifier must reject it; never return synthetic success.
                if (memcmp(dst, e.modified + i, end - i) == 0) {
                    memcpy(dst, e.original + i, end - i);
                    restored += end - i;
                }
                i = end;
            }
        }
        return restored;
    }
};
}
