#!/usr/bin/env python3
"""Execute WMI channel event -> cache -> HAL -> native getters/LQM paths."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]

def function(source,marker):
    start=source.index(marker);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

backend=(root/'qwz/qwz_signal.cpp').read_text()
driver=(root/'controller/ItlQwz.cpp').read_text()
qwz=(root/'qwz/qwz.c').read_text()
ui=(root/'airport/AirportItlwmSkywalkInterface.cpp').read_text()
bodies='\n'.join(function(backend,m) for m in (
    'static int\nqwz_signal_index(', 'void\nqwz_signal_record(',
    'int16_t\nqwz_signal_noise(', 'int\nqwz_signal_busy('))
bodies+='\n'+function(qwz,'void\nqwz_chan_info_event(')
bodies+='\n'+'\n'.join(function(driver,m) for m in (
    'int16_t\nItlQwz::getBSSNoise()', 'int16_t\nItlQwz::getChannelNoise(',
    'int\nItlQwz::getChannelBusy('))
bodies+='\n'+'\n'.join(function(ui,m) for m in (
    'IOReturn AirportItlwmSkywalkInterface::\ngetNOISE(',
    'IOReturn AirportItlwmSkywalkInterface::\ngetRSSI('))
prefix=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include "QwzSignal.hpp"
#include "WCL.hpp"
constexpr int IEEE80211_S_RUN=4,IEEE80211_S_INIT=0;
constexpr int ATH12K_SCAN_IDLE=0,ATH12K_SCAN_STARTING=1,ATH12K_SCAN_RUNNING=2,ATH12K_SCAN_ABORTING=3;
constexpr int WMI_CHAN_INFO_START_RESP=0,WMI_CHAN_INFO_END_RESP=1;
struct Channel {unsigned ic_freq=0,ic_flags=0;};
static Channel anyChannel;
#define IEEE80211_CHAN_ANYC (&anyChannel)
struct Node {Channel *ni_chan=nullptr;uint8_t ni_rssi=0;};
struct ieee80211com {Channel ic_channels[256];int ic_state=IEEE80211_S_RUN;Node *ic_bss=nullptr;};
struct qwz_vif {unsigned vdev_id=0;};
struct qwz_survey_info {int8_t noise;uint64_t time,time_busy;};
struct qwz_softc {
    ieee80211com sc_ic;qwz_vif sc_vif;bool fw_initialized=true,dead=false;
    QwzSignal::Reading sc_signal[256]{};qwz_survey_info survey[256]{};
    uint32_t sc_fw_epoch=1,sc_signal_events=0,sc_signal_last_freq=0,sc_signal_last_raw_noise=0;
    uint32_t sc_signal_noise_samples=0,sc_signal_busy_samples=0,sc_signal_rejected=0;
    uint32_t sc_signal_wmi_events=0,sc_signal_last_flags=0,sc_signal_last_error=0;
    uint32_t cc_freq_hz=0,sc_log_evt=0,mon_freq=5745;
    bool mon_coex=false,mon_active=false,mon_native_owned=false,mon_dwell_inflight=false;
    struct {int state=ATH12K_SCAN_RUNNING;} scan;
    struct {const char *dv_xname="test";} sc_dev;
};
static uint64_t nowNS=0;
static uint64_t getnsecuptime(){return nowNS;}
#define QWZ_CRASHED(sc) ((sc)->dead)
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define DNPRINTF(...) ((void)0)
template<class... T> static void qwz_log(const char *,T...) {}
static bool qwz_log_ratelimit(unsigned *,unsigned,unsigned){return false;}
static int ieee80211_mhz2ieee(unsigned f,int) {
    if(f==5955)return 1; // Deliberately alias 6 GHz channel 1 to test frequency validation.
    if(f>=2412 && f<=2472)return (f-2407)/5;
    return f>=5000 && f<5900 ? (f-5000)/5 : 0;
}
struct wmi_chan_info_event {
    uint32_t err_code=0,freq=5180,cmd_flags=0,noise_floor=uint32_t(-92);
    uint32_t rx_clear_count=70,cycle_count=1000,mac_clk_mhz=0,vdev_id=0;
};
struct mbuf {wmi_chan_info_event event;int parseError=0;};
#define mtod(m,t) reinterpret_cast<t>(m)
#define M_PKTLEN(m) sizeof(*(m))
static int qwz_pull_chan_info_ev(qwz_softc *,uint8_t *data,size_t,wmi_chan_info_event *out) {
    auto m=reinterpret_cast<mbuf *>(data);*out=m->event;return m->parseError;
}
int16_t qwz_signal_noise(qwz_softc *,uint32_t);
int qwz_signal_busy(qwz_softc *,uint32_t);
struct ItlQwz {
    qwz_softc *sc;
    int16_t getBSSNoise();int16_t getChannelNoise(uint32_t);int getChannelBusy(uint32_t);
};
using IOReturn=int;
constexpr int APPLE80211_VERSION=1,APPLE80211_UNIT_DBM=0,kIOReturnSuccess=0;
struct apple80211_noise_data {
    uint32_t version,num_radios,noise_unit;int32_t noise[4],aggregate_noise,noise_ext[4],aggregate_noise_ext;
};
struct apple80211_rssi_data {
    uint32_t version,num_radios,rssi_unit;int32_t rssi[4],aggregate_rssi,rssi_ext[4],aggregate_rssi_ext;
};
struct HAL {
    ItlQwz driver;ieee80211com *get80211Controller(){return &driver.sc->sc_ic;}
    ItlQwz *getDriverInfo(){return &driver;}
};
struct AirportItlwmSkywalkInterface {
    HAL *fHalService;
    int8_t rssiForNode(Node *n){return int8_t(n->ni_rssi);}
    IOReturn getNOISE(apple80211_noise_data *);IOReturn getRSSI(apple80211_rssi_data *);
};
'''
main=r'''
int main() {
    qwz_softc sc;sc.sc_ic.ic_channels[36]={5180,1};sc.sc_ic.ic_channels[149]={5745,1};
    sc.sc_ic.ic_channels[1]={2412,1};Node bss{&sc.sc_ic.ic_channels[36],uint8_t(-55)};sc.sc_ic.ic_bss=&bss;
    ItlQwz driver{&sc};HAL hal{driver};AirportItlwmSkywalkInterface ui{&hal};
    assert(!driver.getBSSNoise() && driver.getChannelBusy(5180)==-1);
    apple80211_noise_data noise{};assert(ui.getNOISE(&noise)!=0);
    mbuf m;qwz_chan_info_event(&sc,&m); // Exact production handler; zero counter clock must not divide by zero.
    assert(driver.getBSSNoise()==-92 && driver.getChannelBusy(5180)==7);
    m.event.noise_floor=0xa4;qwz_chan_info_event(&sc,&m);assert(driver.getBSSNoise()==-92);
    assert(!ui.getNOISE(&noise) && noise.version==1 && noise.noise_unit==APPLE80211_UNIT_DBM && noise.aggregate_noise==-92);
    apple80211_rssi_data rssi{};assert(!ui.getRSSI(&rssi) && rssi.version==1 && rssi.aggregate_rssi==-55);
    bss.ni_rssi=0;assert(ui.getRSSI(&rssi)!=0);bss.ni_rssi=uint8_t(-128);assert(ui.getRSSI(&rssi)!=0);
    bss.ni_rssi=uint8_t(-55);assert(ui.getRSSI(nullptr)!=0);
    // A ROC measurement on 149 must never become the connected channel's noise.
    sc.scan.state=ATH12K_SCAN_IDLE;sc.mon_active=sc.mon_coex=sc.mon_native_owned=sc.mon_dwell_inflight=true;
    m.event.freq=5745;m.event.noise_floor=uint32_t(-104);qwz_chan_info_event(&sc,&m);
    assert(driver.getChannelNoise(5745)==-104 && driver.getBSSNoise()==-92);
    // A real home-channel report on ROC return is valid even after the
    // infrastructure scan state became IDLE; it is keyed to its own freq.
    m.event.freq=5180;m.event.noise_floor=uint32_t(-92);
    qwz_chan_info_event(&sc,&m);assert(driver.getBSSNoise()==-92);
    const unsigned accepted=sc.sc_signal_noise_samples;
    sc.scan.state=ATH12K_SCAN_RUNNING;
    m.event.vdev_id=9;qwz_chan_info_event(&sc,&m);assert(sc.sc_signal_noise_samples==accepted);
    m.event.vdev_id=0;m.event.err_code=1;qwz_chan_info_event(&sc,&m);assert(sc.sc_signal_noise_samples==accepted);
    m.event.err_code=0;m.event.cmd_flags=1;qwz_chan_info_event(&sc,&m);assert(sc.sc_signal_noise_samples==accepted);
    m.event.cmd_flags=0;m.parseError=1;qwz_chan_info_event(&sc,&m);assert(sc.sc_signal_noise_samples==accepted);m.parseError=0;
    // Invalid sentinels never refresh the prior noise timestamp.
    nowNS=QwzSignal::maxAgeNS;
    for(uint32_t v:{0u,0xffffffffu,0xffffff80u,127u,uint32_t(-130)}) {
        m.event.noise_floor=v;qwz_chan_info_event(&sc,&m);assert(sc.sc_signal_noise_samples==accepted);
    }
    assert(driver.getBSSNoise()==-92);++nowNS;assert(driver.getBSSNoise()==0 && ui.getNOISE(&noise)!=0);
    assert(driver.getChannelBusy(5180)==7); // Busy had real independent fresh counters.
    m.event.noise_floor=uint32_t(-96);m.event.cycle_count=1000;m.event.rx_clear_count=0;
    qwz_chan_info_event(&sc,&m);assert(driver.getBSSNoise()==-96 && driver.getChannelBusy(5180)==0);
    ++sc.sc_fw_epoch;assert(!driver.getBSSNoise() && driver.getChannelBusy(5180)==-1);
    qwz_chan_info_event(&sc,&m);assert(driver.getBSSNoise()==-96);
    sc.dead=true;assert(!driver.getBSSNoise());sc.dead=false;sc.fw_initialized=false;assert(!driver.getBSSNoise());sc.fw_initialized=true;
    sc.sc_ic.ic_state=IEEE80211_S_INIT;assert(!driver.getBSSNoise());sc.sc_ic.ic_state=IEEE80211_S_RUN;
    unsigned rejected=sc.sc_signal_rejected;qwz_signal_record(&sc,5955,uint32_t(-80),100,50);
    assert(sc.sc_signal_rejected==rejected+1 && !driver.getChannelNoise(5955));
    // Ratio arithmetic remains bounded at full 32-bit counter values.
    QwzSignal::Reading huge{};QwzSignal::record(huge,5180,1,0,uint32_t(-90),0xffffffffu,0xffffffffu);
    assert(QwzSignal::busy(huge,5180,1,0)==100);
    QwzSignal::Reading bad{};QwzSignal::record(bad,5180,1,0,0,0,1);assert(QwzSignal::busy(bad,5180,1,0)==-1);
    QwzSignal::record(bad,5180,1,0,0,100,101);assert(QwzSignal::busy(bad,5180,1,0)==-1);
    assert(!QwzSignal::record(bad,5180,1,0,0x123400a4,0,0));
    // Actual native LQM constructor: no invented CCA or SNR. The beacon counters stay valid and equal:
    // WCL leaves the network after 60 s without them (recorded on f2s, missed beacons timeout).
    AirportWCL::LqmBeaconUpdate u;
    assert(AirportWCL::makeBeaconUpdate(8,-55,-92,u));
    assert(u.rssiValid && u.noiseValid && u.snrValid && u.snr==37 && !u.ccaValid);
    assert(u.receivedDelta==8 && u.scheduledDelta==8 && u.beaconValid && u.countersValid && u.snapshotValid);
    assert(AirportWCL::makeBeaconUpdate(8,-55,0,u,0) && !u.noiseValid && !u.snrValid && u.ccaValid && u.cca==0);
    assert(AirportWCL::makeBeaconUpdate(8,-100,-80,u,101) && !u.snrValid && !u.ccaValid);
    assert(AirportWCL::makeBeaconUpdate(8,0,-1,u) && !u.rssiValid && !u.noiseValid && !u.snrValid);
    assert(!AirportWCL::makeBeaconUpdate(0,-55,-92,u));
    puts("actual WMI scan/ROC -> fresh channel/epoch cache -> HAL/getter/LQM, invalid values and unavailable state: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-signal-') as directory:
    src=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    src.write_text(prefix+bodies+main)
    subprocess.run(['xcrun','clang++','-std=c++14','-I'+str(root/'include'),'-I'+str(root/'airport/WCL'),'-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
