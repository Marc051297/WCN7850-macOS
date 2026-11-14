// SPDX-License-Identifier: MIT
// Qualcomm HCI policy for the existing IntelBTPatcher hook. No synthetic events.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace QcaBluetooth {
inline bool supportedBuild(const char *build) {
    const char *known[] = {"25G332", "25G342"};
    if (!build) return false;
    for (const char *candidate : known) {
        size_t i = 0;
        while (candidate[i] && candidate[i] == build[i]) ++i;
        if (!candidate[i] && !build[i]) return true;
    }
    return false;
}
enum class BuildGate { KernelTag, OsBuild, UnsupportedKernel, UnsupportedBuild };
inline bool verifiedKernel(const char *kernel) {
    // Both signed 25G332/25G342 kernel collections use this exact XNU tag.
    // Unlike osversion, the exported version string exists from kernel startup.
    const char *tag = "xnu-12377.161.15.701.48~2/RELEASE_X86_64";
    if (!kernel) return false;
    for (const char *p = kernel; *p; ++p) {
        size_t i = 0;
        while (tag[i] && p[i] == tag[i]) ++i;
        if (!tag[i] && (!p[i] || p[i] == '\n')) return true;
    }
    return false;
}
inline BuildGate buildGate(const char *kernel, const char *build) {
    if (!verifiedKernel(kernel)) return BuildGate::UnsupportedKernel;
    // launchd/user space may not have filled kern.osversion when Lilu starts.
    // The exact kernel fingerprint is the early-boot fallback, never the major version.
    if (!build || !*build) return BuildGate::KernelTag;
    return supportedBuild(build) ? BuildGate::OsBuild : BuildGate::UnsupportedBuild;
}
inline bool allowed(BuildGate gate) {
    return gate == BuildGate::KernelTag || gate == BuildGate::OsBuild;
}
inline const char *gateReason(BuildGate gate) {
    switch (gate) {
    case BuildGate::KernelTag: return "verified-kernel-tag";
    case BuildGate::OsBuild: return "verified-os-build-and-kernel";
    case BuildGate::UnsupportedKernel: return "unsupported-kernel";
    case BuildGate::UnsupportedBuild: return "unsupported-os-build";
    }
    return "unknown";
}
inline bool targetDevice(uint32_t vendor, uint32_t product) {
    // Start with the board that has actual firmware/pairing evidence.
    return vendor == 0x0489 && product == 0xe10a;
}
inline bool command(const uint8_t *bytes, size_t size) {
    return bytes && size >= 3 && size == size_t(bytes[2]) + 3;
}
inline uint16_t opcode(const uint8_t *bytes) {
    return uint16_t(bytes[0]) | uint16_t(bytes[1]) << 8;
}
inline bool scanAddress(const uint8_t *address) {
    const uint8_t fixed[] = {0x94, 0x50, 0x64, 0xd0, 0x78, 0x6b};
    for (size_t i = 0; i < 6; ++i)
        if (address[i] != fixed[i]) return false;
    return true;
}
struct State {
    uint8_t general[6] {};
    uint8_t addresses[32][6] {};
    uint32_t knownSets {};
    uint32_t addressedSets {};
    bool haveGeneral {};
    bool scanInitialized {};
};
struct Action {
    uint8_t bytes[10] {};
    size_t size {};
};
inline void copyAddress(uint8_t *to, const uint8_t *from) {
    for (size_t i = 0; i < 6; ++i) to[i] = from[i];
}
// Called after a synchronous submission or an asynchronous USB completion succeeds.
// Submission is not an HCI acknowledgement; counters must not claim otherwise.
inline Action submitted(State &state, const uint8_t *bytes, size_t size) {
    Action action {};
    if (!command(bytes, size)) return action;
    switch (opcode(bytes)) {
    case 0x0c03: // HCI Reset
        state = State{};
        break;
    case 0x2005: // LE Set Random Address
        if (size == 9 && !scanAddress(bytes + 3)) {
            copyAddress(state.general, bytes + 3);
            state.haveGeneral = true;
        }
        break;
    case 0x2035: // LE Set Advertising Set Random Address
        // macOS may send this before creating the set; remember and replay after 0x2036.
        if (size == 10 && bytes[3] < 32 && !scanAddress(bytes + 4)) {
            copyAddress(state.addresses[bytes[3]], bytes + 4);
            state.knownSets |= uint32_t(1) << bytes[3];
        }
        break;
    case 0x2036: // LE Set Extended Advertising Parameters
        if (size == 28 && bytes[3] < 32 && bytes[13] == 1) {
            const uint32_t bit = uint32_t(1) << bytes[3];
            if (!(state.addressedSets & bit) && ((state.knownSets & bit) || state.haveGeneral)) {
                action.bytes[0] = 0x35;
                action.bytes[1] = 0x20;
                action.bytes[2] = 7;
                action.bytes[3] = bytes[3];
                copyAddress(action.bytes + 4, (state.knownSets & bit) ? state.addresses[bytes[3]] : state.general);
                action.size = 10;
            }
        }
        break;
    case 0x203c: // LE Remove Advertising Set
        if (size == 4 && bytes[3] < 32) {
            const uint32_t bit = uint32_t(1) << bytes[3];
            state.knownSets &= ~bit;
            state.addressedSets &= ~bit;
            for (auto &b : state.addresses[bytes[3]]) b = 0;
        }
        break;
    case 0x203d: // LE Clear Advertising Sets
        state.knownSets = state.addressedSets = 0;
        for (auto &address : state.addresses)
            for (auto &b : address) b = 0;
        break;
    default: break;
    }
    return action;
}
inline void addressSubmitted(State &state, const Action &action) {
    if (action.size == 10 && action.bytes[3] < 32)
        state.addressedSets |= uint32_t(1) << action.bytes[3];
}
inline bool needsScanAddress(const State &state, const uint8_t *bytes, size_t size) {
    return command(bytes, size) && opcode(bytes) == 0x200b && size == 10 && !state.scanInitialized;
}
inline bool widenEventMask(uint8_t *bytes, size_t size) {
    if (!command(bytes, size) || opcode(bytes) != 0x2001 || size != 11) return false;
    // Keep all requested bits, add Connection Parameter Request, Data Length Change,
    // Enhanced Connection Complete, PHY Update Complete and Extended Advertising
    // Report (bits 5, 6, 9, 11, 12). Extended Create Connection (0x2043), enabled
    // by our bluetoothd path, completes through the enhanced event (0x0a).
    const bool changed = (bytes[3] & 0x60) != 0x60 || (bytes[4] & 0x1a) != 0x1a;
    bytes[3] |= 0x60;
    bytes[4] |= 0x1a;
    return changed;
}
}
