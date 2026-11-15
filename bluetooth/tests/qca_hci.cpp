#include "QcaBluetoothHci.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace QcaBluetooth;

int main() {
    assert(supportedBuild("25G332") && !supportedBuild("25G241") && supportedBuild("25G342"));
    assert(!supportedBuild(nullptr) && !supportedBuild("25G332x") && !supportedBuild("26B118"));
    const char *kernel = "Darwin Kernel Version 25.6.0: date; root:xnu-12377.161.15.701.48~2/RELEASE_X86_64";
    assert(buildGate(kernel, "25G342") == BuildGate::OsBuild);
    assert(buildGate(kernel, "25G332") == BuildGate::OsBuild);
    assert(buildGate(kernel, "") == BuildGate::KernelTag);
    assert(buildGate(kernel, nullptr) == BuildGate::KernelTag);
    assert(buildGate(kernel, "25G999") == BuildGate::UnsupportedBuild);
    assert(!allowed(buildGate(kernel, "25G999")));
    assert(!allowed(buildGate(nullptr, "25G342")));
    assert(!allowed(buildGate("xnu-12377.161.15.700.19~2/RELEASE_X86_64", "25G342")));
    assert(!allowed(buildGate("xnu-12377.161.15.701.48~20/RELEASE_X86_64", "25G342")));
    assert(!allowed(buildGate("xnu-12377.161.15.701.48~2/RELEASE_ARM64", "25G342")));
    assert(!allowed(buildGate("xnu-12377.161.15.701.48~2/RELEASE_X86_64_DEBUG", "25G342")));
    assert(allowed(buildGate(kernel, "")) && !allowed(buildGate(kernel, "26B118")));
    assert(targetDevice(0x0489, 0xe10a) && !targetDevice(0x8087, 0xe10a) && !targetDevice(0x0489, 0xe10b));
    State state {};
    uint8_t params[28] = {0x36, 0x20, 25, 3};
    params[13] = 1;
    assert(submitted(state, params, sizeof(params)).size == 0); // never invent an address
    uint8_t fixed[] = {0x05,0x20,6,0x94,0x50,0x64,0xd0,0x78,0x6b};
    submitted(state, fixed, sizeof(fixed));
    assert(!state.haveGeneral && submitted(state, params, sizeof(params)).size == 0);
    uint8_t general[] = {0x05,0x20,6,1,2,3,4,5,0xc6};
    submitted(state, general, sizeof(general));
    auto action = submitted(state, params, sizeof(params));
    assert(action.size == 10 && action.bytes[3] == 3 && !memcmp(action.bytes + 4, general + 3, 6));
    assert(submitted(state, params, sizeof(params)).size == 10); // failed extra submission retries
    addressSubmitted(state, action);
    assert(submitted(state, params, sizeof(params)).size == 0);
    uint8_t reset[] = {0x03,0x0c,0};
    submitted(state, reset, sizeof(reset));
    assert(!state.haveGeneral && !state.addressedSets && !state.knownSets && !state.scanInitialized);
    uint8_t perSet[] = {0x35,0x20,7,3,6,5,4,3,2,0xc1};
    submitted(state, general, sizeof(general));
    submitted(state, perSet, sizeof(perSet));
    action = submitted(state, params, sizeof(params));
    assert(action.size == 10 && !memcmp(action.bytes + 4, perSet + 4, 6)); // per-set beats general
    addressSubmitted(state, action);
    uint8_t remove[] = {0x3c,0x20,1,3};
    submitted(state, remove, sizeof(remove));
    assert(!state.knownSets && !state.addressedSets && state.addresses[3][0] == 0);
    submitted(state, reset, sizeof(reset));
    submitted(state, perSet, sizeof(perSet));
    uint8_t clear[] = {0x3d,0x20,0};
    submitted(state, clear, sizeof(clear));
    assert(submitted(state, params, sizeof(params)).size == 0); // stale set address cannot return
    perSet[3] = 32;
    submitted(state, perSet, sizeof(perSet));
    params[3] = 32;
    assert(!state.knownSets && submitted(state, params, sizeof(params)).size == 0);
    params[3] = 3;
    params[13] = 0;
    submitted(state, general, sizeof(general));
    assert(submitted(state, params, sizeof(params)).size == 0); // public address does not need replay
    params[13] = 1;
    for (size_t i = 0; i < sizeof(params); ++i) assert(submitted(state, params, i).size == 0);
    uint8_t invalid[] = {0x35,0x20,7};
    assert(!command(nullptr, 0) && !command(invalid, sizeof(invalid)));
    uint8_t mask[] = {0x01,0x20,8,0x1f,0,0x80,0,0,0,0,0xff};
    assert(widenEventMask(mask, sizeof(mask)) && mask[3] == 0x7f && mask[4] == 0x1a);
    assert(mask[5] == 0x80 && mask[10] == 0xff && !widenEventMask(mask, sizeof(mask)));
    assert(!widenEventMask(mask, sizeof(mask)-1));
    // The old expanded mask is still missing the completion for 0x2043.
    uint8_t oldMask[] = {0x01,0x20,8,0x7f,0x18,0,0,0,0,0,0};
    assert(widenEventMask(oldMask, sizeof(oldMask)) && oldMask[4]==0x1a);
    for (unsigned bit=0;bit<64;++bit) {
        uint8_t one[] = {0x01,0x20,8,0,0,0,0,0,0,0,0};
        one[3+bit/8]=uint8_t(1U<<(bit%8));
        widenEventMask(one,sizeof(one));
        assert(one[3+bit/8] & (1U<<(bit%8))); // every requested bit survives
        assert((one[4]&0x02)!=0); // real Enhanced Connection Complete
    }
    uint8_t scan[] = {0x0b,0x20,7,0,0,0,0,0,0,0};
    assert(needsScanAddress(state, scan, sizeof(scan)));
    state.scanInitialized = true;
    assert(!needsScanAddress(state, scan, sizeof(scan)));
    submitted(state, reset, sizeof(reset));
    assert(needsScanAddress(state, scan, sizeof(scan)));
    std::puts("PASS: Qualcomm HCI bounds, privacy, address rotation, retry, reset and event masks");
}
