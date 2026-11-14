#!/usr/bin/env python3
"""Compile real boot-country/WMI/event code with delayed firmware responses."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "qwz/qwz.c").read_text()


def function(name):
    match = re.search(r"static (?:int|void)\n" + name + r"\(", source)
    assert match, name
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


code = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#define FIELD_PREP(mask,value) ((uint32_t(value)<<__builtin_ctz(mask))&(mask))
constexpr uint32_t WMI_TLV_TAG=0xffff0000U,WMI_TLV_LEN=0xffffU;
constexpr unsigned WMI_TAG_SET_INIT_COUNTRY_CMD=123,WMI_SET_INIT_COUNTRY_CMDID=456;
constexpr unsigned WMI_COUNTRY_INFO_TYPE_ALPHA=0,TLV_HDR_SIZE=4;
constexpr int REG_SET_CC_STATUS_PASS=0;
#define SEC_TO_NSEC(n) (uint64_t(n)*1000000000ULL)
struct ath12k_htc_hdr {uint32_t words[2];};
struct wmi_cmd_hdr {uint32_t word;};
struct wmi_init_country_cmd {
    uint32_t tlv_header,pdev_id,init_cc_type;
    union {uint32_t alpha2,country_code,regdom_id;} cc_info;
};
struct cur_regulatory_info {int status_code=0;char alpha2[3]{};};
struct qwz_pdev_wmi {int id=0;};
struct qwz_softc {
    char reg_country[3]{},reg_country_requested[3]{};
    int reg_country_pending=0,reg_country_request_error=0;
    uint32_t reg_country_status=UINT32_MAX,reg_country_updates=0;
    struct {uint32_t pdev_id=3;} pdevs[1];
    struct {qwz_pdev_wmi wmi[8];} wmi;
    struct {struct {uint32_t ic_flags=0x200,ic_freq=5745;} ic_channels[256];} sc_ic;
};
struct mbuf {uint8_t bytes[sizeof(ath12k_htc_hdr)+sizeof(wmi_cmd_hdr)+sizeof(wmi_init_country_cmd)];};
#define mtod(m,type) reinterpret_cast<type>((m)->bytes)
static std::string argument;
static int sends,frees,sleeps,wakes,publishes,sendResult,publishedError;
static bool allocFail;
enum Reply {Inline,OnWait,Rejected,Unrelated,Absent};
static Reply reply=OnWait;
static qwz_softc *active;
static wmi_init_country_cmd lastCommand;
static void qwz_reg_country_event(qwz_softc *,const cur_regulatory_info *);
static void emit() {
    cur_regulatory_info event;
    memcpy(event.alpha2,reply==Unrelated?"00":"US",3);
    if(reply==Rejected) event.status_code=1;
    if(reply==Inline || reply==OnWait) active->sc_ic.ic_channels[149].ic_flags=0;
    qwz_reg_country_event(active,&event);
}
static bool PE_parse_boot_argn(const char *name,void *out,int length) {
    assert(strcmp(name,"itlwm_cc")==0 && length==8);
    if(argument.empty()) return false;
    std::strncpy(static_cast<char *>(out),argument.c_str(),length-1); return true;
}
static mbuf *qwz_wmi_alloc_mbuf(size_t n) {
    assert(n==sizeof(wmi_init_country_cmd));
    if(allocFail) return nullptr;
    auto *m=new mbuf; memset(m->bytes,0xa5,sizeof(m->bytes)); return m;
}
static void m_freem(mbuf *m) {++frees;delete m;}
static int qwz_wmi_cmd_send(qwz_pdev_wmi *wmi,mbuf *m,uint32_t id) {
    assert(id==WMI_SET_INIT_COUNTRY_CMDID && wmi==&active->wmi.wmi[3]);
    ++sends;
    memcpy(&lastCommand,m->bytes+sizeof(ath12k_htc_hdr)+sizeof(wmi_cmd_hdr),sizeof(lastCommand));
    assert(lastCommand.pdev_id==3 && lastCommand.init_cc_type==WMI_COUNTRY_INFO_TYPE_ALPHA);
    assert(lastCommand.tlv_header==(WMI_TAG_SET_INIT_COUNTRY_CMD<<16 | (sizeof(lastCommand)-4)));
    assert(lastCommand.cc_info.alpha2==uint32_t('U'|('S'<<8))); // no stale padding bytes
    if(sendResult) return sendResult; // caller retains ownership on failure
    m_freem(m); // successful send transfers ownership
    if(reply==Inline) emit();
    return 0;
}
static void wakeup(void *p) {assert(p==&active->reg_country_pending);++wakes;}
static int tsleep_nsec(void *p,int flags,const char *label,uint64_t ns) {
    assert(p==&active->reg_country_pending && !flags && ns==1000000000ULL);
    assert(strcmp(label,"qwzcountry")==0); ++sleeps;
    if(reply==Absent) return ETIMEDOUT;
    emit();
    return active->reg_country_pending?ETIMEDOUT:0;
}
static void qwz_log(const char *,...) {}
static void qwz_compat_publish_country(const char *,const char *,int,int error,uint32_t,uint32_t,uint32_t) {
    ++publishes;publishedError=error;
}
'''
code += "\n" + "\n".join(function(n) for n in (
    "qwz_reg_country_event", "qwz_wmi_set_init_country", "qwz_apply_boot_country"))
code += r'''
static void reset(qwz_softc &sc) {
    sc=qwz_softc{}; active=&sc; sends=frees=sleeps=wakes=publishes=sendResult=publishedError=0;
    allocFail=false;argument="US";reply=OnWait;
}
int main() {
    qwz_softc sc;
    for(auto invalid:{"","ZZ","zz","00","U","USA","U1"," US"}) {
        reset(sc);argument=invalid;qwz_apply_boot_country(&sc);
        assert(!sends && !sleeps && !sc.reg_country_requested[0]);
    }
    reset(sc);argument="us";qwz_apply_boot_country(&sc);
    assert(sends==1 && frees==1 && sleeps==1 && wakes==1);
    assert(!strcmp(sc.reg_country,"US") && !strcmp(sc.reg_country_requested,"US"));
    assert(sc.reg_country_updates==1 && !sc.reg_country_pending && !publishedError);
    assert(!sc.sc_ic.ic_channels[149].ic_flags);
    // Same firmware country: no redundant command or wait, diagnostics still update.
    qwz_apply_boot_country(&sc);assert(sends==1 && sleeps==1 && publishes==3);
    reset(sc);reply=Inline;qwz_apply_boot_country(&sc);
    assert(sends==1 && !sleeps && !publishedError && !strcmp(sc.reg_country,"US"));
    reset(sc);reply=Rejected;memcpy(sc.reg_country,"00",3);qwz_apply_boot_country(&sc);
    assert(publishedError==EIO && sc.reg_country_status==1 && !strcmp(sc.reg_country,"00"));
    assert(sc.sc_ic.ic_channels[149].ic_flags==0x200 && !sc.reg_country_pending);
    reset(sc);reply=Unrelated;qwz_apply_boot_country(&sc);
    assert(publishedError==ETIMEDOUT && !wakes && !strcmp(sc.reg_country,"00"));
    assert(sc.sc_ic.ic_channels[149].ic_flags==0x200);
    reset(sc);reply=Absent;qwz_apply_boot_country(&sc);
    assert(publishedError==ETIMEDOUT && sleeps==1 && !sc.reg_country_pending);
    // A late actual firmware response can update permissions after our bounded wait.
    reply=OnWait;emit(); assert(!strcmp(sc.reg_country,"US") && !wakes);
    reset(sc);sendResult=EIO;qwz_apply_boot_country(&sc);
    assert(sends==1 && frees==1 && !sleeps && publishedError==EIO && !sc.reg_country_pending);
    reset(sc);allocFail=true;qwz_apply_boot_country(&sc);
    assert(!sends && !frees && !sleeps && publishedError==ENOMEM && !sc.reg_country_pending);
    puts("ok: country command encoding/ownership, actual firmware replies, rejection, unrelated/late events and bounded timeout");
}
'''
with tempfile.TemporaryDirectory(prefix="qwz-country-") as directory:
    path, binary = Path(directory) / "test.cpp", Path(directory) / "test"
    path.write_text(code)
    subprocess.run(["xcrun", "clang++", "-std=c++14", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
