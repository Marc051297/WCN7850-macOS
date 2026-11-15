#!/usr/bin/env python3
"""Exercise actual WCL latch and WMI scan producer with a synthetic tether list."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'qwz/qwz.c').read_text()
start=s.index('\tif (ic->ic_wcl_scan_requested &&\n\t    AirportWCL::scanUsesProbes')
end=s.index('\n#ifdef AIRPORT_WCL',start)
producer="#ifdef AIRPORT_WCL\n"+s[start:end]
w=(root/'airport/WCL/WCL.cpp').read_text()
start=w.index('    ic->ic_wcl_scan_ssid_length =')
end=w.index('    ic->ic_wcl_scan_restricted =',start)
latch=w[start:end]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include "WCL.hpp"
#define AIRPORT_WCL 1
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
constexpr unsigned IEEE80211_NWID_LEN=32,WMI_SCAN_FLAG_PASSIVE=1;
struct Station {bool ic_wcl_scan_requested=true,ic_wcl_scan_active=true;
 uint8_t ic_wcl_scan_ssid_length=0,ic_wcl_scan_ssid[32]{};
 uint8_t ic_wcl_scan_ssid_count=0,ic_wcl_scan_ssid_lengths[11]{},ic_wcl_scan_ssids[11][32]{};
 uint8_t ic_des_esslen=0,ic_des_essid[32]{};
};
struct Scan {unsigned num_ssids=0,scan_flags=0,scan_f_bcast_probe=0;
 struct {unsigned length=0;uint8_t ssid[32]{};} ssid[16];};
static void latch(Station *ic,const AirportWCL::ScanRequest &scan) {
''' + latch + r'''
}
static int submit(Station *ic,Scan *arg) {
 int ret=0;
''' + producer + r'''
exit:return ret;
}
int main(){
 uint8_t request[0x1550]{}; request[0x40]=1;request[0x1318]=2;
 request[0x131c]=1;request[0x1320]=7;memcpy(request+0x1324,"example",7);
 AirportWCL::ScanRequest decoded{};assert(AirportWCL::decodeScan(request,sizeof(request),decoded));
 Station ic;Scan cmd;latch(&ic,decoded);assert(submit(&ic,&cmd)==0);
 assert(cmd.num_ssids==2 && cmd.ssid[0].length==7 && !memcmp(cmd.ssid[0].ssid,"example",7));
 assert(cmd.ssid[1].length==0 && cmd.scan_f_bcast_probe==1 && !(cmd.scan_flags&WMI_SCAN_FLAG_PASSIVE));
 // Passive scans never transmit supplied names; ordinary legacy primary still works.
 decoded.type=2;latch(&ic,decoded);ic.ic_wcl_scan_active=false;cmd=Scan{};
 assert(submit(&ic,&cmd)==0 && !cmd.num_ssids && (cmd.scan_flags&WMI_SCAN_FLAG_PASSIVE));
 ic.ic_wcl_scan_active=true;ic.ic_wcl_scan_ssid_length=3;memcpy(ic.ic_wcl_scan_ssid,"old",3);cmd=Scan{};
 assert(submit(&ic,&cmd)==0 && cmd.num_ssids==1 && cmd.ssid[0].length==3 && !memcmp(cmd.ssid[0].ssid,"old",3));
 // A malformed internal length cannot overflow the WMI command's SSID storage.
 ic.ic_wcl_scan_ssid_count=1;ic.ic_wcl_scan_ssid_lengths[0]=33;cmd=Scan{};
 assert(submit(&ic,&cmd)==EINVAL);
 puts("native scan request -> latch -> firmware named/wildcard probes: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-scan-probes-') as directory:
    src=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    src.write_text(code)
    subprocess.run(['xcrun','clang++','-std=c++14','-DAIRPORT_WCL','-I'+str(root/'airport/WCL'),'-I'+str(root/'include'),'-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
