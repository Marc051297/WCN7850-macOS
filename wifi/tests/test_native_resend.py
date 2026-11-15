#!/usr/bin/env python3
"""RS (f2u): native AirDrop data frames the receiver did not acknowledge are sent again, in order.

The production resend list (qwz_mon.c: the qwz_mon_rs_* helpers and the three qwz_mon_native_resend_* entry
points), the production statements of the completion handler that keep a frame, the production statements of the
submit path that record a frame's history, and the production resend loop of the native pump
(QwzAWDLRadio.cpp) run here against a small firmware and receiver model: frames complete in the order they were
submitted, acknowledged while the receiver is on the channel and unacknowledged while it is away.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
mon = (root / 'qwz/qwz_mon.c').read_text()
radio = (root / 'airport/awdlnative/QwzAWDLRadio.cpp').read_text()


def function(name):
    m = re.search(r'(?:static )?(?:int|void)\n' + name + r'\(', mon)
    assert m, name
    end = mon.index('{', m.end()) + 1
    depth = 1
    while depth:
        depth += (mon[end] == '{') - (mon[end] == '}')
        end += 1
    return mon[m.start():end]


def braces(text, start):
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


helpers = mon[mon.index('#define QWZ_MON_RS_WINDOW_NS'):mon.index('qwz_mon_tx_compl(struct qwz_softc *sc, struct wmi_mgmt_tx_compl_event *ev)')]
helpers = helpers[:helpers.rindex('\n}') + 2]   # up to the end of qwz_mon_rs_keep, without the next function's head
assert 'qwz_mon_rs_keep' in helpers and 'qwz_mon_rs_flush' in helpers
# The completion handler's statements from the status test to the slot's release.
c0 = mon.index('\tif (sc->mon_tx_kind[ev->desc_id] == 2) {\n\t\tif (ev->status == 0)')
completion = mon[c0:mon.index('\ttd->m = NULL;\n', c0) + len('\ttd->m = NULL;\n')]
assert 'kept = qwz_mon_rs_keep(sc, ev->desc_id, td->m, getnsecuptime());' in completion
assert 'if (!kept)\n\t\tm_freem(td->m);' in completion
# The submit path's statements that give a slot its frame's history.
history = braces(mon, mon.index('\tif (slot >= 0 && slot < (int)nitems(sc->mon_tx_tries)) {'))
assert 'nativeMode >= 0 && kind == 2' in history
# The pump's resend loop.
loop = braces(radio, radio.index('\t\t{\n\t\t\tuint8_t destination[6];'))
assert 'qwz_mon_native_resend_send' in loop and 'awdlNativePeerTiming.hold' in loop
# The purge and the cell start must free what waits; the reaper must forget the slot's history.
assert 'qwz_mon_rs_flush(sc);\t/* RS (f2u): nor does a frame waiting for another try */' in mon
assert 'qwz_mon_rs_flush(sc);\t/* RS (f2u): nothing waits from an earlier cell */' in mon
assert 'sc->mon_tx_tries[slot] = 0;\t/* RS (f2u): a frame whose completion never came is not sent again */' in mon

code = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define IEEE80211_ADDR_EQ(a,b) (memcmp((a),(b),6)==0)
#define IEEE80211_ADDR_COPY(d,s) memcpy((d),(s),6)
constexpr unsigned QWZ_MON_RS_MAX=64;
struct mbuf {std::vector<uint8_t> bytes;};
static int allocated,freed;
static mbuf *m_new(const uint8_t *f,size_t n) {++allocated;auto *m=new mbuf;m->bytes.assign(f,f+n);return m;}
static void m_freem(mbuf *m) {assert(m);++freed;delete m;}
#define mtod(m,type) reinterpret_cast<type>((m)->bytes.data())
#define M_PKTLEN(m) ((m)->bytes.size())
static uint64_t clockNS=1000000000ULL;
static uint64_t getnsecuptime() {return clockNS;}
static void bus_dmamap_unload(int,int) {}
struct qwz_softc {
    int mon_active=1,mon_coex_halt=0,sc_dmat=0;
    struct qwz_mon_resend {mbuf *m;uint64_t first_ns,notbefore_ns;uint32_t serial;uint8_t tries;} mon_rs[QWZ_MON_RS_MAX]{};
    unsigned mon_rs_count=0;
    uint32_t mon_rs_serial=0,mon_rs_ctx_serial=0;
    uint8_t mon_rs_ctx_tries=0;
    uint64_t mon_rs_ctx_first_ns=0;
    uint8_t mon_tx_tries[64]{},mon_tx_kind[64]{};
    uint32_t mon_tx_serial[64]{};
    uint64_t mon_tx_first_ns[64]{};
    struct {uint32_t ufr_sent=0,ufr_full=0,ufr_fail=0,ufr_data_noack=0,ufr_data_resent=0,ufr_data_gaveup=0,ufr_data_rsfull=0;
            uint64_t ufr_data_acked_bytes=0;} mon_stats;
    struct {struct {mbuf *m=nullptr;int map=0;} data[64];} txq;
};
static qwz_softc *qwz_mon_sc;
static int qwz_nativeresend=6;
static int txReady=1,slotsFree=1;
static int qwz_mon_native_tx_ready() {return txReady;}
static std::deque<unsigned> inFlight;   // slot ids in the order the firmware took them
static unsigned busyNotes;
static void qwz_mon_busy_note(qwz_softc *) {++busyNotes;}   // SD (f2v): every native data submission counts toward "a transfer is running"
''' + helpers + r'''
// The submit path in miniature: its own copy of the frame in a free slot of the cell's ring (ids 8 to 63), then the
// production statements that record the frame's history in that slot.
static int qwz_mon_tx_bytes(qwz_softc *sc,const uint8_t *frame,size_t len,uint8_t kind,int nativeMode=-1) {
    int slot=-1;
    if(!slotsFree) return ENOSPC;
    for(int i=8;i<64;++i) if(!sc->txq.data[i].m) {slot=i;break;}
    if(slot<0) return ENOSPC;
    sc->txq.data[slot].m=m_new(frame,len);
    sc->mon_tx_kind[slot]=kind;
''' + history + r'''
    inFlight.push_back(unsigned(slot));
    return 0;
}
''' + '\n'.join(function(n) for n in ('qwz_mon_native_resend_first', 'qwz_mon_native_resend_blocks', 'qwz_mon_native_resend_send')) + r'''
// The completion handler's own statements around one firmware completion event.
static void complete(qwz_softc *sc,unsigned id,unsigned status) {
    struct {unsigned desc_id,status;} event{id,status},*ev=&event;
    auto *td=&sc->txq.data[id];
    int kept=0;
    assert(td->m);
''' + completion + r'''
    sc->mon_tx_kind[id]=0;
}
// A data frame from us to `to`, tagged; 60 bytes.
static std::vector<uint8_t> frame(uint8_t to,uint8_t tag,bool group=false) {
    std::vector<uint8_t> f(60,0);
    f[0]=0x08;f[4]=group?0x01:0x02;f[9]=to;f[10]=0x02;f[15]=0x77;f[22]=tag;f[23]=0;f[40]=tag;
    return f;
}
static bool present[256];                       // is the receiver (address byte) on the channel
static std::vector<uint8_t> delivered[256];     // tags each receiver took, in order; a retry it already has is discarded
static std::vector<uint8_t> retryBits[256];
// The firmware finishes the oldest `n` frames in flight, in submission order.
static void air(qwz_softc *sc,unsigned n=1000) {
    while(n-- && !inFlight.empty()) {
        const unsigned id=inFlight.front();inFlight.pop_front();
        const uint8_t *f=sc->txq.data[id].m->bytes.data();
        const uint8_t to=f[9],tag=f[40];
        const bool ok=present[to];
        if(ok) {
            bool have=false;for(uint8_t t:delivered[to]) have|=t==tag;
            if(!have) {delivered[to].push_back(tag);retryBits[to].push_back(f[1]&0x08);}
        }
        complete(sc,id,ok?0:3);
    }
}
static int send(qwz_softc *sc,uint8_t to,uint8_t tag) {
    auto f=frame(to,tag);return qwz_mon_tx_bytes(sc,f.data(),f.size(),2,0);
}
// The pump's own resend loop, with its surroundings.
struct Stats {uint64_t dataResent=0;};
struct Self {
    bool awdlNativePeerTimingEnabled=true;
    uint64_t awdlNativeLastDataUS=0;
    struct Timing {bool hold(const uint8_t *d,uint8_t,uint64_t) {return !present[d[5]];}} awdlNativePeerTiming;
};
static int qwz_nativepeerpause=1;
static uint64_t nowUS() {return clockNS/1000;}
static int qwz_mon_tx_room() {return slotsFree?56:0;}
static unsigned pump(Self *self,unsigned budget=16) {
    Stats st;const uint64_t now=nowUS();const int cell=149;
''' + loop + r'''
    return unsigned(st.dataResent);
}
static void reset(qwz_softc &sc) {
    qwz_mon_rs_flush(&sc);
    for(auto &d:sc.txq.data) if(d.m) {m_freem(d.m);d.m=nullptr;}
    inFlight.clear();
    sc.~qwz_softc();new(&sc) qwz_softc;qwz_mon_sc=&sc;
    for(auto &d:delivered) d.clear();
    for(auto &r:retryBits) r.clear();
    memset(present,0,sizeof(present));
    qwz_nativeresend=6;txReady=slotsFree=1;clockNS+=5000000000ULL;
}
static std::vector<uint8_t> tags(uint8_t from,uint8_t to) {std::vector<uint8_t> v;for(unsigned t=from;t<=to;++t)v.push_back(uint8_t(t));return v;}
int main() {
    qwz_softc sc;qwz_mon_sc=&sc;Self self;
    uint8_t A[6]={0x02,0,0,0,0,4},B[6]={0x02,0,0,0,0,6},dest[6];
    const uint8_t a=4,b=6;

    // 1. A burst cut off when the receiver leaves: the frames it missed wait, in order, and reach it when it is back.
    reset(sc);present[a]=true;
    for(uint8_t t=1;t<=10;++t) assert(send(&sc,a,t)==0);
    air(&sc,4);present[a]=false;air(&sc);
    assert(delivered[a]==tags(1,4) && sc.mon_rs_count==6 && sc.mon_stats.ufr_data_noack==6);
    assert(qwz_mon_native_resend_blocks(A) && !qwz_mon_native_resend_blocks(B));
    assert(qwz_mon_native_resend_first(0,dest)==0 && !memcmp(dest,A,6) && qwz_mon_native_resend_first(1,dest)==-1);
    assert(pump(&self)==0 && sc.mon_rs_count==6 && inFlight.empty());   // the receiver is away: nothing is spent
    present[a]=true;
    assert(pump(&self)==6 && sc.mon_rs_count==0 && inFlight.size()==6);
    air(&sc);
    assert(delivered[a]==tags(1,10) && !qwz_mon_native_resend_blocks(A));
    for(unsigned n=0;n<10;++n) assert(retryBits[a][n]==(n<4?0:0x08));    // the second copies carry the Retry bit
    assert(sc.mon_stats.ufr_data_resent==6 && !sc.mon_stats.ufr_data_gaveup && sc.mon_stats.ufr_sent==6);
    assert(sc.mon_stats.ufr_data_acked_bytes==10*60);

    // 2. A resent frame that fails again goes back to its place, ahead of the newer frames still waiting.
    reset(sc);
    for(uint8_t t=1;t<=6;++t) assert(send(&sc,a,t)==0);
    air(&sc);assert(sc.mon_rs_count==6);
    present[a]=true;assert(pump(&self,3)==3 && sc.mon_rs_count==3);     // 1, 2, 3 in flight; 4, 5, 6 wait
    air(&sc,1);present[a]=false;air(&sc);                               // 1 arrives, 2 and 3 are missed again
    assert(sc.mon_rs_count==5);
    for(unsigned n=0;n<5;++n) assert(sc.mon_rs[n].m->bytes[40]==n+2 && sc.mon_rs[n].tries==(n<2?2:1));
    clockNS+=6000000;present[a]=true;                                    // past the 5 ms pause after a second failure
    assert(pump(&self)==5);air(&sc);
    assert(delivered[a]==tags(1,6) && sc.mon_rs_count==0);

    // 3. After a second failure a frame pauses 5 ms, after a third 25 ms; meanwhile it and the frames behind it stay.
    reset(sc);assert(send(&sc,a,1)==0 && send(&sc,a,2)==0);air(&sc);
    assert(sc.mon_rs_count==2 && qwz_mon_native_resend_send(0)==0);air(&sc);   // sent again at once, missed again
    assert(sc.mon_rs_count==2 && sc.mon_rs[0].tries==2 && sc.mon_rs[0].m->bytes[40]==1);
    assert(qwz_mon_native_resend_send(0)==EAGAIN);
    present[a]=true;assert(pump(&self)==0 && inFlight.empty());          // frame 2 does not pass frame 1
    clockNS+=5000000;assert(qwz_mon_native_resend_send(0)==0);
    present[a]=false;air(&sc);assert(sc.mon_rs[0].tries==3);
    clockNS+=24000000;assert(qwz_mon_native_resend_send(0)==EAGAIN);
    clockNS+=1000000;present[a]=true;assert(pump(&self)==2);air(&sc);
    assert(delivered[a]==tags(1,2));

    // 4. The bounds. `nativeresend` submissions, then the frame is dropped as before; 700 ms from the first submission.
    reset(sc);qwz_nativeresend=3;assert(send(&sc,a,1)==0);
    for(int n=0;n<2;++n) {air(&sc);assert(sc.mon_rs_count==1);clockNS+=30000000;assert(qwz_mon_native_resend_send(0)==0);}
    air(&sc);
    assert(sc.mon_rs_count==0 && sc.mon_stats.ufr_data_gaveup==1 && sc.mon_stats.ufr_data_noack==3 && sc.mon_stats.ufr_data_resent==2);
    reset(sc);assert(send(&sc,a,1)==0 && send(&sc,a,2)==0);air(&sc);
    clockNS+=699000000;assert(qwz_mon_native_resend_send(0)==0);         // still inside the window
    clockNS+=1000000;assert(qwz_mon_native_resend_send(0)==ETIMEDOUT && sc.mon_rs_count==0 && sc.mon_stats.ufr_data_gaveup==1);
    air(&sc);assert(sc.mon_rs_count==0 && sc.mon_stats.ufr_data_gaveup==2);   // the late failure of frame 1 is past the window too
    reset(sc);assert(send(&sc,a,1)==0);air(&sc);
    qwz_nativeresend=0;assert(qwz_mon_native_resend_send(0)==ETIMEDOUT && sc.mon_rs_count==0);   // switched off while a frame waited
    assert(send(&sc,a,2)==0);air(&sc);assert(sc.mon_rs_count==0 && sc.mon_stats.ufr_data_gaveup==1);   // off: sent once and released as up to f2t, not counted as given up
    assert(qwz_mon_native_resend_send(0)==ENXIO);

    // 5. Only unicast data of the native lane waits. A group frame, the user-space lane, a management frame, a frame
    // acknowledged or discarded by the firmware are released exactly as before.
    reset(sc);
    {auto f=frame(a,1,true);assert(qwz_mon_tx_bytes(&sc,f.data(),f.size(),2,0)==0);}
    {auto f=frame(a,2);assert(qwz_mon_tx_bytes(&sc,f.data(),f.size(),2)==0);}          // the user-space lane
    {auto f=frame(a,3);f[0]=0xd0;assert(qwz_mon_tx_bytes(&sc,f.data(),f.size(),1,0)==0);}
    air(&sc);assert(sc.mon_rs_count==0 && sc.mon_stats.ufr_data_noack==2 && !sc.mon_stats.ufr_data_gaveup);
    assert(send(&sc,a,4)==0);{const unsigned id=inFlight.front();inFlight.pop_front();complete(&sc,id,1);}   // discard
    assert(sc.mon_rs_count==0);
    // A cell that is stopping keeps nothing.
    assert(send(&sc,a,5)==0);sc.mon_coex_halt=1;air(&sc);assert(sc.mon_rs_count==0);sc.mon_coex_halt=0;
    assert(send(&sc,a,6)==0);sc.mon_active=0;air(&sc);assert(sc.mon_rs_count==0);
    assert(qwz_mon_native_resend_first(0,dest)==-1 && !qwz_mon_native_resend_blocks(A) && qwz_mon_native_resend_send(0)==ENXIO);
    sc.mon_active=1;

    // 6. The list is bounded: 64 frames wait, the next unacknowledged one is dropped and counted.
    reset(sc);
    for(unsigned t=0;t<65;++t) {assert(send(&sc,a,uint8_t(t))==0);air(&sc);}
    assert(sc.mon_rs_count==64 && sc.mon_stats.ufr_data_rsfull==1 && sc.mon_rs[63].m->bytes[40]==63);

    // 7. Two receivers: one away with frames waiting does not keep the other's from going; each keeps its own order.
    reset(sc);
    for(uint8_t t=1;t<=3;++t) {assert(send(&sc,a,t)==0);assert(send(&sc,b,uint8_t(10+t))==0);}
    air(&sc);assert(sc.mon_rs_count==6);
    present[b]=true;
    assert(qwz_mon_native_resend_first(0,dest)==0 && !memcmp(dest,A,6));
    assert(qwz_mon_native_resend_first(1,dest)==1 && !memcmp(dest,B,6) && qwz_mon_native_resend_first(2,dest)==-1);
    assert(pump(&self)==3);air(&sc);
    assert(delivered[b]==tags(11,13) && delivered[a].empty() && sc.mon_rs_count==3);
    assert(qwz_mon_native_resend_blocks(A) && !qwz_mon_native_resend_blocks(B));
    // No slot or credit: the frame keeps waiting and nothing is counted as sent again.
    present[a]=true;slotsFree=0;assert(pump(&self)==0 && sc.mon_rs_count==3);
    assert(qwz_mon_native_resend_send(0)==ENOSPC && sc.mon_stats.ufr_full==1);
    slotsFree=1;txReady=0;assert(qwz_mon_native_resend_send(0)==ENOSPC && sc.mon_rs_count==3);
    txReady=1;
    // The holds switched off: the waiting frames go without asking the schedule.
    present[a]=false;self.awdlNativePeerTimingEnabled=false;assert(pump(&self)==3);
    self.awdlNativePeerTimingEnabled=true;present[a]=true;air(&sc);
    assert(delivered[a]==tags(1,3));

    // 8. A receiver that heard the first copy but whose acknowledgement was lost gets the same frame with the Retry
    // bit and the same sequence bytes, so it keeps one copy.
    reset(sc);assert(send(&sc,a,1)==0);
    {const unsigned id=inFlight.front();inFlight.pop_front();
     delivered[a].push_back(1);retryBits[a].push_back(0);complete(&sc,id,3);}
    assert(sc.mon_rs_count==1);
    const std::vector<uint8_t> first=sc.mon_rs[0].m->bytes;
    present[a]=true;assert(pump(&self)==1);
    {const auto &again=sc.txq.data[inFlight.front()].m->bytes;
     assert(again.size()==first.size() && again[1]==(first[1]|0x08) && again[22]==first[22] && again[23]==first[23]);
     for(size_t n=2;n<again.size();++n) assert(again[n]==first[n]);}
    air(&sc);assert(delivered[a]==tags(1,1));

    // 9. The purge frees everything that waits, and every frame is freed exactly once overall.
    reset(sc);
    for(uint8_t t=1;t<=5;++t) assert(send(&sc,a,t)==0);
    air(&sc);assert(sc.mon_rs_count==5);
    qwz_mon_rs_flush(&sc);
    assert(sc.mon_rs_count==0 && !sc.mon_rs[0].m && !sc.mon_tx_tries[8] && !sc.mon_rs_ctx_tries);
    reset(sc);
    assert(allocated==freed);
    assert(busyNotes>100);   // first submissions and resends alike
    puts("ok: unacknowledged native data frames wait in order, go again when their receiver is back, keep their place after another miss, and stay bounded");
}
'''
with tempfile.TemporaryDirectory(prefix='qwz-native-resend-') as directory:
    path, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    path.write_text(code)
    subprocess.run(['xcrun', 'clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
