#!/usr/bin/env python3
"""Run the actual regulatory parsers and dispatcher on bounded wire fixtures."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "qwz/qwz.c").read_text()
header = (root / "qwz/qwzreg.h").read_text()


def definition(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if marker.startswith("struct "):
        end = text.index(";", end) + 1
    return text[start:end]


code = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define __packed __attribute__((packed))
#define DNPRINTF(...) ((void)0)
#define FIELD_GET(mask,value) (((value)&(mask))>>__builtin_ctz(mask))
constexpr unsigned WMI_TLV_LEN=0xffff,WMI_TLV_TAG=0xffff0000U;
constexpr unsigned WMI_TAG_REG_CHAN_LIST_CC_EVENT=1,WMI_TAG_REG_CHAN_LIST_CC_EXT_EVENT=2;
constexpr unsigned WMI_TAG_ARRAY_STRUCT=3,WMI_TAG_MAX=4,MAX_REG_RULES=10,REG_ALPHA2_LEN=2;
constexpr unsigned WMI_REG_CLIENT_MAX=4,WMI_REG_CURRENT_MAX_AP_TYPE=3,WMI_REG_MAX_CLIENT_TYPE=2;
constexpr int REG_SET_CC_STATUS_PASS=0;
enum cc_setting_code {Pass=0,Rejected=1};
enum wmi_reg_cc_setting_code {WPass=0,WRejected=1};
enum wmi_reg_6ghz_client_type {Client=0};
enum wmi_reg_chan_list_cmd_type {WMI_REG_CHAN_LIST_CC_ID=0,WMI_REG_CHAN_LIST_CC_EXT_ID=1};
'''
for name in ("cur_reg_rule", "cur_regulatory_info", "wmi_reg_chan_list_cc_event",
             "wmi_regulatory_rule_struct", "wmi_reg_chan_list_cc_ext_event", "wmi_regulatory_ext_rule"):
    code += "\n" + definition(header, "struct " + name + " {")
code += r'''
struct mbuf {std::vector<uint8_t> bytes;};
#define mtod(m,type) reinterpret_cast<type>((m)->bytes.data())
#define M_PKTLEN(m) ((m)->bytes.size())
struct qwz_softc {struct {const char *dv_xname="mock";} sc_dev; int reg_country_rules_valid=0;};
static unsigned worlds,inits,replies,lastStatus;
static char lastCountry[3];
static const void **qwz_wmi_tlv_parse_alloc(qwz_softc *,const void *data,size_t length) {
    auto *tb=static_cast<const void **>(calloc(WMI_TAG_MAX,sizeof(void *)));
    auto *p=static_cast<const uint8_t *>(data);
    while(length) {
        uint32_t tlv;
        if(length<4) {free(tb);return nullptr;}
        memcpy(&tlv,p,4);p+=4;length-=4;
        unsigned tag=tlv>>16,n=tlv&0xffff;
        size_t minimum=tag==1?sizeof(wmi_reg_chan_list_cc_event):tag==2?sizeof(wmi_reg_chan_list_cc_ext_event):0;
        if(n>length || n<minimum) {free(tb);return nullptr;}
        if(tag<WMI_TAG_MAX) tb[tag]=p;
        p+=n;length-=n;
    }
    return tb;
}
static cc_setting_code qwz_wmi_cc_setting_code_to_reg(wmi_reg_cc_setting_code s) {return s==WPass?Pass:Rejected;}
static void qwz_log(const char *,...) {}
static void qwz_print_reg_rule(qwz_softc *,const char *,unsigned,cur_reg_rule *) {}
static cur_reg_rule *qwz_create_reg_rules_from_wmi(unsigned n,wmi_regulatory_rule_struct *p) {
    for(unsigned i=0;i<n;++i) assert(p[i].freq_info==0x1234);
    return static_cast<cur_reg_rule *>(calloc(n,sizeof(cur_reg_rule)));
}
static cur_reg_rule *qwz_create_ext_reg_rules_from_wmi(unsigned n,wmi_regulatory_ext_rule *p) {
    for(unsigned i=0;i<n;++i) assert(p[i].freq_info==0x1234);
    return static_cast<cur_reg_rule *>(calloc(n,sizeof(cur_reg_rule)));
}
static uint8_t qwz_wmi_ignore_num_extra_rules(wmi_regulatory_ext_rule *,unsigned) {return 0;}
static void qwz_init_channels_world(qwz_softc *) {++worlds;}
static void qwz_init_channels(qwz_softc *,cur_regulatory_info *) {++inits;}
static void qwz_reg_country_event(qwz_softc *,const cur_regulatory_info *info) {
    ++replies;lastStatus=info->status_code;memcpy(lastCountry,info->alpha2,2);
}
#define free(p,zone,size) std::free(p)
#define malloc(size,zone,flags) static_cast<cur_regulatory_info *>(std::calloc(1,size))
'''
for name, signature in (
    ("qwz_reg_rule_array", "static const void *\n"),
    ("qwz_pull_reg_chan_list_update_ev", "int\n"),
    ("qwz_pull_reg_chan_list_ext_update_ev", "int\n"),
    ("qwz_reg_chan_list_event", "int\n"),
):
    code += "\n" + definition(source, signature + name + "(")
code += r'''
#undef free
#undef malloc
template<class Header,class Rule> static mbuf packet(unsigned tag,unsigned status,unsigned count,bool rules=true) {
    mbuf m;Header h{};h.alpha2='U'|('S'<<8);h.status_code=status;
    h.num_2ghz_reg_rules=count;
    uint32_t tlv=(tag<<16)|sizeof(h);
    m.bytes.resize(4+sizeof(h));memcpy(m.bytes.data(),&tlv,4);memcpy(m.bytes.data()+4,&h,sizeof(h));
    if(rules) {
        tlv=WMI_TAG_ARRAY_STRUCT<<16 | count*sizeof(Rule);
        auto offset=m.bytes.size();m.bytes.resize(offset+4+count*sizeof(Rule));
        memcpy(m.bytes.data()+offset,&tlv,4);
        for(unsigned i=0;i<count;++i) {
            Rule r{};r.freq_info=0x1234;
            memcpy(m.bytes.data()+offset+4+i*sizeof(r),&r,sizeof(r));
        }
    }
    return m;
}
template<class Header,class Rule> static void run(unsigned tag,wmi_reg_chan_list_cmd_type id) {
    qwz_softc sc;
    auto valid=packet<Header,Rule>(tag,0,1);
    assert(qwz_reg_chan_list_event(&sc,&valid,id)==0 && sc.reg_country_rules_valid);
    unsigned before=inits,worldBefore=worlds,replyBefore=replies;
    // Firmware rejection and status-only success must not erase valid rules.
    auto rejected=packet<Header,Rule>(tag,1,0,false);
    assert(qwz_reg_chan_list_event(&sc,&rejected,id)==0);
    assert(lastStatus==1 && !strcmp(lastCountry,"US"));
    auto empty=packet<Header,Rule>(tag,0,0,false);
    assert(qwz_reg_chan_list_event(&sc,&empty,id)==0 && lastStatus==0);
    assert(inits==before && worlds==worldBefore && replies==replyBefore+2);
    // Truncated arrays and lying counts fail safely, retaining the old table.
    auto shortArray=packet<Header,Rule>(tag,0,2);
    shortArray.bytes.resize(shortArray.bytes.size()-sizeof(Rule));
    assert(qwz_reg_chan_list_event(&sc,&shortArray,id)!=0 && worlds==worldBefore);
    auto missing=packet<Header,Rule>(tag,0,1,false);
    assert(qwz_reg_chan_list_event(&sc,&missing,id)!=0 && worlds==worldBefore);
    auto huge=packet<Header,Rule>(tag,0,MAX_REG_RULES+1);
    assert(qwz_reg_chan_list_event(&sc,&huge,id)!=0 && worlds==worldBefore);
    // First boot with no valid rules stays conservative.
    sc.reg_country_rules_valid=0;
    assert(qwz_reg_chan_list_event(&sc,&empty,id)==0 && worlds==worldBefore+1);
}
int main() {
    run<wmi_reg_chan_list_cc_event,wmi_regulatory_rule_struct>(1,WMI_REG_CHAN_LIST_CC_ID);
    run<wmi_reg_chan_list_cc_ext_event,wmi_regulatory_ext_rule>(2,WMI_REG_CHAN_LIST_CC_EXT_ID);
    puts("ok: both wire parsers retain country/rejection metadata, bound rule arrays, preserve valid permissions");
}
'''
with tempfile.TemporaryDirectory(prefix="qwz-reg-reply-") as directory:
    path, binary = Path(directory) / "test.cpp", Path(directory) / "test"
    path.write_text(code)
    subprocess.run(["xcrun", "clang++", "-std=c++14", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
