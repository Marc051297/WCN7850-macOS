#include <cassert>
#include <cstdio>
#include <vector>
#include "WCL.hpp"

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
    puts("ok: directed/list-bearing hotspot scans require the radio; wildcard refresh keeps cached behavior");
}
