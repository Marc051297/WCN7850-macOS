#include <cassert>
#include <cstdio>
#include <vector>
#include "WCL.hpp"

static void word(std::vector<uint8_t> &b, size_t off, uint32_t value) {
    for(unsigned i=0;i<4;++i)b[off+i]=uint8_t(value>>(i*8));
}

int main()
{
    AirportWCL::ScanRequest scan{};
    std::vector<uint8_t> request(0x1550, 0);
    assert(!AirportWCL::scanNeedsRadio(scan, request.data(), request.size()));
    assert(!AirportWCL::scanNeedsRadio(scan, nullptr, 0));
    // The real tether request has no primary SSID, two list entries and
    // one selected channel. It must reach firmware despite a live AWDL cell.
    request[0x1318] = 2;
    assert(AirportWCL::scanNeedsRadio(scan, request.data(), request.size()));
    assert(!AirportWCL::scanNeedsRadio(scan, request.data(), 0x131b));
    std::vector<uint8_t> shortRequest(0x58, 0);
    assert(!AirportWCL::scanNeedsRadio(scan, shortRequest.data(), shortRequest.size()));
    scan.ssidLength = 1;
    assert(AirportWCL::scanNeedsRadio(scan, shortRequest.data(), shortRequest.size()));
    // 25G342's captured tether form: an empty primary SSID, version-one
    // named entry and all-zero wildcard, each 56 bytes. Use synthetic names.
    std::fill(request.begin(),request.end(),0);
    word(request,0x40,1);word(request,0x54,1);
    word(request,0x5c,44);word(request,0x60,0x92);
    word(request,0x1318,2);word(request,0x131c,1);word(request,0x1320,7);
    memcpy(request.data()+0x1324,"example",7);
    assert(AirportWCL::decodeScan(request.data(),request.size(),scan));
    assert(!scan.ssidLength && scan.probeSSIDCount==2);
    assert(scan.probeSSIDs[0].length==7 && !memcmp(scan.probeSSIDs[0].bytes,"example",7));
    assert(scan.probeSSIDs[1].length==0);
    assert(AirportWCL::scanChannelSelected(scan.channels,true,44));
    assert(!AirportWCL::scanChannelSelected(scan.channels,true,6));
    // Primary/list duplicates are emitted once, wildcard remains explicit.
    word(request,0x1c,7);memcpy(request.data()+0x20,"example",7);
    assert(AirportWCL::decodeScan(request.data(),request.size(),scan) && scan.probeSSIDCount==2);
    word(request,0x1c,5);memcpy(request.data()+0x20,"other",5);
    assert(AirportWCL::decodeScan(request.data(),request.size(),scan) && scan.probeSSIDCount==3);
    // Unsupported versions, oversized lengths/counts and short lists fail
    // transactionally rather than becoming unintended wildcard scans.
    auto invalid=request;
    for(unsigned size: {33U,0xffffffffU}) {
        invalid=request;word(invalid,0x1320,size);scan.identifier=77;
        assert(!AirportWCL::decodeScan(invalid.data(),invalid.size(),scan) && scan.identifier==77);
    }
    invalid=request;word(invalid,0x131c,2);
    assert(!AirportWCL::decodeScan(invalid.data(),invalid.size(),scan));
    invalid=request;word(invalid,0x1318,11);
    assert(!AirportWCL::decodeScan(invalid.data(),invalid.size(),scan));
    assert(!AirportWCL::decodeScan(request.data(),0x131c+56*2-1,scan));
    invalid=request;invalid[0x131c+56+8]=1; // version-zero nonempty is not a wildcard
    assert(!AirportWCL::decodeScan(invalid.data(),invalid.size(),scan));
    // Ten distinct list entries plus a different primary fit bounded storage.
    word(request,0x1318,10);
    for(unsigned k=0;k<10;++k) {
        const size_t off=0x131c+56*k;word(request,off,1);word(request,off+4,1);
        request[off+8]=uint8_t(k);memset(request.data()+off+9,0,47);
    }
    assert(AirportWCL::decodeScan(request.data(),request.size(),scan) && scan.probeSSIDCount==11);
    // A short legacy request has no list tail and retains its primary form.
    assert(AirportWCL::decodeScan(request.data(),0x64,scan) && scan.probeSSIDCount==1);
    puts("ok: directed hotspot scan lists preserve named/wildcard probes, limits and primary deduplication");
}
