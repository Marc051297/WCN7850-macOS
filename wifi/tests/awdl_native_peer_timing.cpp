#include <AWDLNativePeerTiming.hpp>
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace ItlAWDL;

static size_t action(uint8_t *f, unsigned window, unsigned subtype, const uint8_t *seq, unsigned id=1) {
    memset(f, 0, 160);
    f[0]=0xd0; f[10]=2; f[15]=uint8_t(id);
    const uint8_t bssid[]={0,0x25,0,0xff,0x94,0x73}; memcpy(f+16,bssid,6);
    const uint8_t header[]={0x7f,0,0x17,0xf2,8,0x10}; memcpy(f+24,header,6); f[30]=subtype;
    f[40]=4; f[41]=33;
    auto *v=f+43; v[0]=149;v[1]=54;v[3]=149;v[5]=16;v[27]=4;
    v[29]=uint8_t(window*4);v[30]=uint8_t(window*4>>8);
    f[76]=18;f[77]=38;v=f+79;
    v[0]=15;v[1]=3;v[3]=3;v[4]=v[5]=255;
    for(unsigned k=0;k<16;++k){v[6+2*k]=seq[k];v[7+2*k]=seq[k] ? (seq[k]<=14 ? 81 : 124) : 0;}
    return 117;
}
int main() {
    NativePeerTiming timing{};
    uint8_t seq[16]={};seq[2]=seq[9]=149;
    uint8_t f[160],dest[]={2,0,0,0,0,1},other[]={2,0,0,0,0,2};
    constexpr uint64_t epoch=10000000,E=65536;
    // PSF discovery frames really arrive during away slots. They may anchor
    // the clock but must never turn such slots into evidence of availability.
    for(unsigned w=0;w<200;++w) {
        const auto n=action(f,w,0,seq);
        assert(timing.noteAction(f,n,149,epoch+w*E+10000));
        assert(!timing.hold(dest,149,epoch+w*E+11000));
    }
    assert(!timing.known(epoch+199*E+10000));
    assert(timing.peers[0].mifWindows==0 && timing.peers[0].schedule.binFrames==0);
    timing.reset();
    // Observations establish an arbitrary offset, rather than assuming zero.
    constexpr unsigned offset=5;
    unsigned heard=0,last=0;
    for(unsigned w=0;heard<8;++w) {
        if(seq[(w+offset)%16]!=149) continue;
        const auto n=action(f,w,3,seq);
        assert(timing.noteAction(f,n,149,epoch+w*E+10000));
        ++heard;last=w;
        if(heard<8) assert(!timing.hold(dest,149,epoch+w*E+11000));
        // Duplicate MIFs in a burst cannot manufacture eight windows.
        assert(timing.noteAction(f,n,149,epoch+w*E+10100));
        assert(timing.peers[0].mifWindows==heard);
    }
    auto &p=timing.peers[0];auto &s=p.schedule;
    assert(s.calibrated && s.offset==offset && !s.assumeZero);
    assert(timing.known(epoch+last*E+11000)==1);
    // Only a fully validated AWDL IPv6 data frame feeds the timing evidence.
    uint8_t dataFrame[100]{},ip[40]{};ip[0]=0x60;
    dataFrame[0]=0x08;dataFrame[4]=2;memcpy(dataFrame+10,dest,6);memcpy(dataFrame+16,f+16,6);
    size_t payload=0;
    assert(buildIPv6DataPayload(ip,sizeof(ip),0,dataFrame+24,sizeof(dataFrame)-24,&payload)==BuildResult::Ready);
    assert(timing.noteData(dataFrame,24+payload,false,149,epoch+last*E+12000));
    assert(timing.dataObservations==1);
    dataFrame[1]=0x40;
    assert(!timing.noteData(dataFrame,24+payload,false,149,epoch+last*E+13000));
    dataFrame[1]=0;
    assert(!timing.noteData(dataFrame,24+payload-1,false,149,epoch+last*E+13000));
    assert(!timing.noteData(dataFrame,24+payload,false,44,epoch+last*E+13000));
    uint64_t away=epoch+(last+1)*E+10000;
    while(!s.shouldPause(away)) away+=E;
    assert(timing.hold(dest,149,away));
    assert(!timing.hold(other,149,away));
    const uint8_t multicast[]={0x33,0x33,0,0,0,1};
    assert(!timing.hold(multicast,149,away));
    // A stalled timer cannot leave the FIFO held forever. After a forced
    // release, the existing 50 ms holdoff permits forward progress.
    assert(!timing.hold(dest,149,away+kPeerPauseCeilingUS));
    assert(timing.forcedReleases==1);
    assert(!timing.hold(dest,149,away+kPeerPauseCeilingUS+1000));
    assert(!timing.hold(dest,44,away+kPeerPauseCeilingUS+2000));
    assert(!timing.hold(dest,149,s.heardUS+kPeerScheduleStaleUS));
    // Two independent contradictory MIF windows revoke calibration and open
    // the gate. PSFs never caused the contradiction above.
    unsigned contradictions=0;
    for(unsigned w=last+1;contradictions<2;++w) {
        if(seq[(w+offset)%16]==149)continue;
        const auto n=action(f,w,3,seq);
        assert(timing.noteAction(f,n,149,epoch+w*E+10000));++contradictions;
    }
    assert(!s.calibrated);
    assert(!timing.hold(dest,149,s.heardUS+1000));
    // Truncation, wrong OUI/BSSID/version, protected actions, group sources
    // and malformed TLVs cannot populate a timing peer.
    timing.reset();size_t n=action(f,2,3,seq);
    for(size_t length=0;length<n;++length)assert(!timing.noteAction(f,length,149,epoch));
    for(unsigned index : {16U,25U,29U,30U,41U}) {
        n=action(f,2,3,seq);f[index]=99;
        assert(!timing.noteAction(f,n,149,epoch));
    }
    n=action(f,2,3,seq);f[1]=0x40;assert(!timing.noteAction(f,n,149,epoch));
    n=action(f,2,3,seq);f[10]=1;assert(!timing.noteAction(f,n,149,epoch));
    assert(!timing.known(epoch));
    // Fixed storage is bounded, eviction is safe, and lifecycle clearing
    // releases all per-peer state after listener/address/channel transitions.
    for(unsigned id=1;id<=20;++id){n=action(f,2,3,seq,id);assert(timing.noteAction(f,n,149,epoch+id));}
    assert(!timing.find(dest));
    timing.clearPeers();
    for(const auto &peer:timing.peers)assert(!peer.occupied);
    puts("native peer timing: PSF separation, calibration, bounded holds and invalidation passed");
}
