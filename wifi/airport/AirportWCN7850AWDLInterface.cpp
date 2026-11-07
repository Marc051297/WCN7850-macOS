//
//  AirportWCN7850AWDLInterface.cpp
//  AirportWCN7850
//
//  Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
//

#include "AirportWCN7850AWDLInterface.hpp"
#include "AirportWCN7850AWDLBridge.h"

/* The Wi-Fi kext's log ring, from the controller's QwzAwdlBridge property; IOLog until init() reads it. */
static struct qwz_awdl_bridge awdlBridge;

static int awdlLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int awdlLog(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    if (awdlBridge.vlog != NULL) {
        r = awdlBridge.vlog(fmt, ap);
    } else {
        IOLogv(fmt, ap);
        r = 0;
    }
    va_end(ap);
    return r;
}

static void awdlPublish(void)
{
    if (awdlBridge.publish != NULL)
        awdlBridge.publish();
}

static bool awdlHookTableMatches(void);
static void awdlGetterAudit(bool answering);

OSDefineMetaClassAndStructors(AirportWCN7850AWDLInterface, IO80211AWDLProtocol);

bool AirportWCN7850AWDLInterface::init(IO80211Controller *controller, ether_addr *address, unsigned int role, char const *name)
{
    OSData *bridge = controller ? OSDynamicCast(OSData, controller->getProperty(QWZ_AWDL_BRIDGE_KEY)) : NULL;

    if (bridge != NULL && bridge->getLength() == sizeof(awdlBridge) &&
        ((const struct qwz_awdl_bridge *)bridge->getBytesNoCopy())->version == QWZ_AWDL_BRIDGE_VERSION)
        memcpy(&awdlBridge, bridge->getBytesNoCopy(), sizeof(awdlBridge));
    else
        IOLog("AWDLProbe: no %s on the controller: logging to IOLog\n", QWZ_AWDL_BRIDGE_KEY);

    memset(hookCalls, 0, sizeof(hookCalls));
    memcpy(awdlAddress, address, sizeof(awdlAddress));
    awdlController = controller;
    /* Firmware-style events (APPLE80211_M_AWDL_SYNC_STATE_CHANGED) from the controller's workloop. */
    syncTimer = IOTimerEventSource::timerEventSource(this, syncTimerFired);
    if (syncTimer != NULL && (controller == NULL || controller->getWorkLoop() == NULL ||
                              controller->getWorkLoop()->addEventSource(syncTimer) != kIOReturnSuccess)) {
        awdlLog("airport: awdl0: no sync timer: sync state changes are not reported\n");
        OSSafeReleaseNULL(syncTimer);
    }
    answering = awdlHookTableMatches();
    if (!answering)
        awdlLog("airport: awdl0: the hook table does not match answer(): every getter stays unsupported\n");
    if (!IO80211SkywalkInterface::init()) {
        awdlLog("airport: awdl0: init() failed\n");
        return false;
    }
    /* Sets the role, allocates the flow queue database, registers for power interest. */
    if (!IO80211VirtualInterface::init(controller, address, 6, "awdl")) {
        awdlLog("airport: awdl0: IO80211VirtualInterface::init(role 6) failed\n");
        return false;
    }
    setInterfaceId(2);
    awdlLog("airport: awdl0: initialised in the AWDL kext, role %d (asked %u, %s), address %02x:%02x:%02x:%02x:%02x:%02x\n",
            getInterfaceRole(), role, name ? name : "-", awdlAddress[0], awdlAddress[1], awdlAddress[2],
            awdlAddress[3], awdlAddress[4], awdlAddress[5]);
    awdlGetterAudit(answering);
    return true;
}

const char *AirportWCN7850AWDLInterface::getBSDNamePrefix()
{
    return "awdl";
}

IOReturn AirportWCN7850AWDLInterface::setMacAddress(ether_addr &address)
{
    memcpy(awdlAddress, &address, sizeof(awdlAddress));
    awdlLog("airport: awdl0 setMacAddress %02x:%02x:%02x:%02x:%02x:%02x\n",
            awdlAddress[0], awdlAddress[1], awdlAddress[2],
            awdlAddress[3], awdlAddress[4], awdlAddress[5]);
    awdlPublish();
    return kIOReturnSuccess;
}

#include "AirportWCN7850AWDLHooksImpl.inc"

/* AirportWCN7850AWDLHooksImpl.inc's indices of the hooks answer() handles, and of 10, which it leaves
 * unanswered on purpose; init() checks the names. */
static const struct { int index; const char *name; } awdlAnswered[] = {
    { 0, "getSSID" }, { 2, "getBSSID" }, { 5, "getAWDL_SYNC_ENABLED" },
    { 9, "getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE" }, { 30, "setAWDL_SYNC_ENABLED" },
    { 35, "setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE" }, { 41, "setAWDL_OOB_REQUEST" },
    { 42, "setAWDL_SYNC_FRAME_TEMPLATE" }, { 50, "setAWDL_OOB_AUTO_REQUEST" },
    { 12, "getAWDL_SYNC_STATE" }, { 37, "setAWDL_SYNC_STATE" },
    { 10, "getPEER_CACHE_MAXIMUM_SIZE" }, { 40, "setPEER_CACHE_CONTROL" },

    { 29, "setAWDL_SYNC_PARAMS" }, { 31, "setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS" }, { 36, "setAWDL_MASTER_CHANNEL" },
    { 39, "setAWDL_PRESENCE_MODE" }, { 45, "setAWDL_SECONDARY_MASTER_CHANNEL" }, { 46, "setAWDL_AF_TX_MODE" },
};

/*
 * apple80211_awdl_oob_request, as the engine fills it (26.7.1 hook dumps):
 * destination at 4, channel (u16) at 0x14, BSSID at 0x16, frame body length
 * at 0x28, pointer to the body (from the category byte) at 0x30.
 * apple80211_awdl_sync_frame_template: body length at 4, pointer at 8.
 */
#define OOB_DEST	0x04
#define OOB_CHANNEL	0x14
#define OOB_BSSID	0x16
#define OOB_LEN		0x28
#define OOB_BODY	0x30
#define AWDL_FRAME_MAX	1600

static bool awdlKernelPointer(const void *p)
{
    return (uintptr_t)p >= 0xffffff8000000000ULL;
}

static void awdlHex(char *out, size_t outLen, const uint8_t *p, size_t n)
{
    size_t i, len = 0;

    out[0] = '\0';
    for (i = 0; i < n && len + 4 < outLen; i++)
        len += snprintf(out + len, outLen - len, "%02x ", p[i]);
}

/*
 * M2a WP5: the setters that describe the sync parameters (hook indices of AirportWCN7850AWDLHooksImpl.inc) go to the
 * Wi-Fi kext as one record through the bridge's tx (repeat 4); it builds TLV 4 from them. Only the words the engine's
 * buffer is known to have are read [HK: the canary offsets of the hook dumps, LOG 7229-7240, 7652, 7669, 7670]:
 * 24 bytes for SYNC_PARAMS, 20 for EXTENSION_STATE_MACHINE_PARAMETERS, 8 for the others. The answer to the engine
 * does not change. No allocation, no wait: the Wi-Fi kext's tx copies the 32 bytes and returns.
 */
static void awdlSetterOut(int index, const uint8_t *d)
{
    struct qwz_awdl_setter_rec rec;
    size_t words, i;

    if (awdlBridge.version < 5 || awdlBridge.tx == NULL)
        return;
    memset(&rec, 0, sizeof(rec));
    rec.magic = QWZ_AWDL_SETTER_MAGIC;
    rec.index = (uint32_t)index;
    words = index == 29 ? 5 : index == 31 ? 4 : 1;
    for (i = 0; i < words; i++)
        memcpy(&rec.v[i], d + 4 + 4 * i, 4);
    awdlBridge.tx(awdlBridge.ctx, (const uint8_t *)&rec, sizeof(rec), QWZ_AWDL_TX_SETTER);
}

/* The BSSID every AWDL frame carries; the firmware forwarded AWDL frames with it (awdltest 4). */
static const uint8_t awdlBssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };

/*
 * What a getter may write into the family's buffer (M2a WP1, the hook audit).
 *
 * A getter's buffer belongs to the caller, and it has the size the family's own handler for that ioctl
 * demands: the API user client's handler of each getter compares the request length with a constant
 * before it routes the request to the driver (apple80211RouteInternal). These constants, read from
 * IO80211Family in the 26.7.2 (25G332) kernel collection, are the sizes below; 0 means no size is known
 * because the getter has no user-client handler. Where the engine's own buffer could also be measured on
 * this hardware, from the stack canary behind it, the two agree: 8 bytes behind
 * getPEER_CACHE_MAXIMUM_SIZE and getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES (the same eight bytes,
 * 29 00 bb d7 17 a5 ea fd on 2026-11-05, follow each; hook 25 has no handler, its 8 is that measurement),
 * and for the setters of PRESENCE_MODE 8, AF_TX_MODE 8, EXTENSION_STATE_MACHINE_PARAMETERS 20 and
 * SYNC_PARAMS 24 (its follower calls). Several are larger than the structures of
 * include/Airport/apple80211_ioctl.h (SYNC_STATE 24 against 8, ELECTION_METRIC 12 against 8, SYNC_PARAMS
 * 24 against 20): those headers are reconstructions.
 *
 * A getter is answered only if awdlGetterAnswered[] lists it, and never with more bytes than its size
 * here: hook() passes no other getter to answer(), and answer() stores only through awdlPut(). Hook 10
 * (getPEER_CACHE_MAXIMUM_SIZE) is not listed on purpose: answered with 16 bytes it overwrote the
 * engine's 8-byte buffer and the canary behind it (awdldata, 2026-11-04). Hook 12 (getAWDL_SYNC_STATE) is
 * answered exactly as in M0.1 (the 16 bytes the engine last set, once it has set them), inside the family's 24.
 */
#define AWDL_GETTERS	27	/* hooks 0..26 of awdlHookNames[] are the getters */
static constexpr uint16_t awdlGetterSize[AWDL_GETTERS] = {
      40,  16,  12,   8,  24,  12,  20,   8,	/*  0 SSID, CHANNEL, BSSID, STATE, SYNC_PARAMS, SYNC_ENABLED, EXTENSION_STATE_MACHINE_PARAMETERS, ELECTION_ID */
      12, 400,   8,   8,  24,  20,   8,  56,	/*  8 ELECTION_METRIC, CHANNEL_SEQUENCE, PEER_CACHE_MAXIMUM_SIZE, MASTER_CHANNEL, SYNC_STATE, ELECTION_RSSI_THRESHOLDS, PRESENCE_MODE, ELECTION_PARAMS */
       0,   8,   8,   8,   8,   0, 124,   0,	/* 16 DEVICE_CAPABILITIES, SECONDARY_MASTER_CHANNEL, AF_TX_MODE, MIN_RATE, ENCRYPTION_TYPE, TXCAL_PERIOD, DEV_STATS, DFSP_CONFIG */
     392,   8,  44,				/* 24 CCA, AF_SECONDARY_PAYLOAD_CAPABILITIES (measured, no handler), LLW_PARAMS */
};
static_assert(sizeof(struct apple80211_ssid_data) == awdlGetterSize[0], "getSSID: the family's apple80211_ssid_data");
static_assert(sizeof(struct apple80211_bssid_data) == awdlGetterSize[2], "getBSSID: the family's apple80211_bssid_data");
static_assert(sizeof(struct apple80211_awdl_sync_enabled) == awdlGetterSize[5], "getAWDL_SYNC_ENABLED: the family accepts exactly 12 bytes");
static_assert(sizeof(struct apple80211_awdl_sync_channel_sequence) == awdlGetterSize[9], "getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE: 0x190");

/* The getters answer() answers. Nothing else is: not 10, not any other. */
static const int awdlGetterAnswered[] = { 0, 2, 5, 9, 12 };

/* The most bytes answer() may store for hook index: the family's request size of an answered getter, 0 for every other hook. */
static size_t awdlGetterMax(int index)
{
    for (size_t i = 0; i < sizeof(awdlGetterAnswered) / sizeof(awdlGetterAnswered[0]); i++)
        if (awdlGetterAnswered[i] == index)
            return awdlGetterSize[index];
    return 0;
}

/*
 * The one place an answer is stored into a getter's buffer: want bytes of src, cut at awdlGetterMax(index).
 * A cut answer is logged (the first four times per hook, then every 64th; calls is the hook's counter).
 * Returns the bytes stored.
 */
static size_t awdlPut(int index, uint32_t calls, void *data, const void *src, size_t want)
{
    size_t max = awdlGetterMax(index), n = want < max ? want : max;

    if (n != 0)
        memcpy(data, src, n);
    if (n < want && (calls <= 4 || (calls & 63) == 0))
        awdlLog("airport: awdl0 %s #%u answer cut: wanted %u bytes, limit %u, stored %u\n", awdlHookNames[index], calls,
                (unsigned)want, (unsigned)max, (unsigned)n);
    return n;
}

/* How much of an incoming getter buffer hook() reads before it answers: the 16 bytes the old dump read, more only inside the family's size, at most 32. */
static size_t awdlInBytes(size_t max)
{
    return max < 16 ? 16 : (max > 32 ? 32 : max);
}

/* A getter's buffer as the family handed it over, before this kext touches it (hook() decides when; max is awdlGetterMax()). */
static void awdlGetterIn(int index, uint32_t n, const uint8_t *p, size_t max)
{
    char hex[3 * 32 + 4], size[8];

    if (index < 0 || index >= AWDL_GETTERS)	/* hook() passes getters only; the table has no entry for a setter */
        return;
    awdlHex(hex, sizeof(hex), p, awdlInBytes(max));
    snprintf(size, sizeof(size), awdlGetterSize[index] != 0 ? "%u" : "?", (unsigned)awdlGetterSize[index]);
    if (max != 0)
        awdlLog("airport: awdl0 %s #%u in (family size %s, answers up to %u) | %s\n", awdlHookNames[index], n, size,
                (unsigned)max, hex);
    else
        awdlLog("airport: awdl0 %s #%u in (family size %s, never answered) | %s\n", awdlHookNames[index], n, size, hex);
}

/*
 * init()'s audit line: the limits in force, from the tables, so the ring shows which AWDL kext is loaded.
 * answering is false when the check of the hook table failed: hook() answers no getter then, and the line says so.
 */
static void awdlGetterAudit(bool answering)
{
    if (!answering) {
        awdlLog("airport: awdl0: hook audit (M2a WP1): no getter is answered (hook table mismatch); every getter answers unsupported\n");
        return;
    }
    awdlLog("airport: awdl0: hook audit (M2a WP1): getters answered up to SSID %u, BSSID %u, SYNC_ENABLED %u, "
            "CHANNEL_SEQUENCE %u, SYNC_STATE %u bytes (the family's own request sizes); hook 10 and every other getter answer unsupported\n",
            (unsigned)awdlGetterMax(0), (unsigned)awdlGetterMax(2), (unsigned)awdlGetterMax(5), (unsigned)awdlGetterMax(9),
            (unsigned)awdlGetterMax(12));
}

/*
 * M2a (WP3, I5, Y0): what the engine hands the setters, logged BEFORE answer() sees it. Log only. It runs with the engine's lock
 * held on any thread, so it formats and logs and nothing else (no gate, no workloop, no allocation).
 *  - setPEER_CACHE_CONTROL (40): the first 0x4c bytes, then the channel-sequence entries the buffer announces (the count byte at
 *    0x45 and 12-byte entries from 0x4c: [KC] IO80211AWDLPeer::fillInCacheParams, one function, not cross-checked against a dump).
 *  - setAWDL_PEER_TRAFFIC_REGISTRATION (44): the service name and the flag (apple80211_awdl_peer_traffic_registration, 172 bytes).
 * The buffer is the engine's and its size is not in our headers, so nothing here reads past the 4 KB page that holds its start:
 * a pointer the engine handed us is mapped, hence so is the rest of its page, and a read inside that page cannot fault whatever
 * the real size of the buffer is. Every read is bounded by `room`, the bytes left in that page; a read the page end would cut is
 * clipped or skipped, and the line says "truncated at page end".
 */
static void awdlEvidence(int index, const void *data, bool set, uint32_t n)
{
    const uint8_t *d = (const uint8_t *)data;
    size_t room;

    if (!set || d == NULL || !awdlKernelPointer(d))
        return;
    room = 0x1000 - ((uintptr_t)d & 0xfff);
    if (index == 40 && (n <= 16 || (n & 15) == 0)) {
        char hex[3 * 80 + 4];
        size_t head = room < 0x4c ? room : 0x4c;
        uint32_t count = 0, i;

        awdlHex(hex, sizeof(hex), d, head);
        awdlLog("airport: awdl0 peer cache control #%u: first 0x%x bytes%s | %s\n", n, (unsigned)head,
                head < 0x4c ? " (truncated at page end)" : "", hex);
        if (room > 0x45)
            count = d[0x45];
        if (room > 0x45 && count >= 1 && count <= 16) {
            /* 12-byte entries from 0x4c: only those that lie wholly inside the page are read */
            uint32_t fit = room > 0x4c ? (uint32_t)((room - 0x4c) / 12) : 0;
            uint32_t shown = count < fit ? count : fit;

            for (i = 0; i < shown; i += 4) {
                char seq[160];
                size_t o = 0;
                uint32_t k;

                seq[0] = '\0';
                for (k = i; k < i + 4 && k < shown && o + 40 < sizeof(seq); k++) {
                    const uint8_t *e = d + 0x4c + 12 * k;
                    uint32_t w0, w1, w2;

                    memcpy(&w0, e, 4);
                    memcpy(&w1, e + 4, 4);
                    memcpy(&w2, e + 8, 4);
                    o += snprintf(seq + o, sizeof(seq) - o, " %u:%x/%x/%x", k, w0, w1, w2);
                }
                awdlLog("airport: awdl0 peer cache control #%u: sequence entries (count %u, words 0/1/2)%s\n", n, count, seq);
            }
            if (shown < count)
                awdlLog("airport: awdl0 peer cache control #%u: truncated at page end: %u of %u entries read\n", n, shown, count);
        } else if (room > 0x45) {
            awdlLog("airport: awdl0 peer cache control #%u: the count byte at 0x45 is %u (not 1 to 16): no entries read\n", n, count);
        } else {
            awdlLog("airport: awdl0 peer cache control #%u: the page ends before the count byte (0x%x bytes left): no entries read\n", n,
                    (unsigned)room);
        }
    }
    if (index == 44 && (n <= 16 || (n & 15) == 0)) {
        char name[48];
        uint32_t nl = 0, active = 0, lim = 0, i;

        if (room >= 16) {
            memcpy(&nl, d + 12, 4);
            lim = nl < 40 ? nl : 40;
            if (lim > room - 16)
                lim = (uint32_t)(room - 16);
        }
        for (i = 0; i < lim; i++)
            name[i] = (d[16 + i] >= 0x20 && d[16 + i] < 0x7f) ? (char)d[16 + i] : '.';
        name[i] = '\0';
        if (room >= 172)
            memcpy(&active, d + 168, 4);
        awdlLog("airport: awdl0 peer traffic registration #%u: name_len %u \"%s\" active %u%s\n", n, nl, name, active,
                room >= 172 ? "" : " (truncated at page end)");
    }
}

/*
 * M2a (WP3, I5): the engine's out-of-band request body, logged whenever its length or contents change (the old line stops after 64
 * requests; the body stayed at 139 bytes in all 60 that M0 logged). TLV types in order, the count of TLV 0x02 (service response) and
 * of TLV 0x10, and an FNV hash. At most 128 lines, then every 16th.
 */
static void awdlOobBody(const uint8_t *body, uint32_t len, bool repeat)
{
    static uint32_t lastLen, lastSum, logs;
    uint32_t sum = 2166136261u, i, nt = 0, n2 = 0, n10 = 0;
    size_t off = 0, l = 0, o = 0;
    char types[3 * 24 + 4];

    types[0] = '\0';
    while (off + 3 <= len) {
        l = body[off + 1] | (body[off + 2] << 8);
        if (off + 3 + l > len)
            break;
        if (nt < 24 && o + 4 < sizeof(types))
            o += snprintf(types + o, sizeof(types) - o, "%s%02x", o ? " " : "", body[off]);
        nt++;
        if (body[off] == 0x02)
            n2++;
        else if (body[off] == 0x10)
            n10++;
        off += 3 + l;
    }
    for (i = 0; i < len; i++) {
        sum ^= body[i];
        sum *= 16777619u;
    }
    if (len == lastLen && sum == lastSum)
        return;
    logs++;
    if (logs <= 128 || (logs & 15) == 0)
        awdlLog("airport: awdl0 OOB body #%u: %s%u bytes (was %u), %u TLVs [%s], TLV 0x02 x%u, TLV 0x10 x%u, sum %08x\n", logs,
                repeat ? "auto " : "", len, lastLen, nt, types, n2, n10, sum);
    lastLen = len;
    lastSum = sum;
}

static bool awdlHookTableMatches(void)
{
    for (size_t i = 0; i < sizeof(awdlAnswered) / sizeof(awdlAnswered[0]); i++)
        if (strcmp(awdlHookNames[awdlAnswered[i].index], awdlAnswered[i].name) != 0)
            return false;
    return true;
}

/*
 * Every hook: count it. A getter's buffer belongs to the family and has the size its own handler demands
 * (awdlGetterSize[]): the first call of every getter (the first two of an answered one) logs what that
 * buffer holds BEFORE this kext touches it, and an answered getter publishes that line at once, so a
 * freeze inside the answer still leaves it on the MacBook. Then answer() it: by default accept a setter,
 * decline a getter; a getter that awdlGetterAnswered[] does not list never reaches answer(), and a listed
 * one is stored only through awdlPut(). Last, log the first four calls and every 64th with the first
 * bytes of the argument as they are after the call (the structure sizes of the setters are unknown, so
 * only a small prefix is read).
 */
IOReturn AirportWCN7850AWDLInterface::hook(int index, const void *data, bool set, size_t dumpBytes)
{
    uint32_t n = ++hookCalls[index];
    awdlEvidence(index, data, set, n);	/* M2a (WP3, I5): what the engine hands the setters, before answer() */
    char hex[3 * 64 + 1];
    const uint8_t *p = (const uint8_t *)data;
    size_t i, len = 0;
    size_t max = (set || !answering) ? 0 : awdlGetterMax(index);
    IOReturn ret = set ? kIOReturnSuccess : kIOReturnUnsupported;

    if (!set && p != NULL && (n == 1 || (n == 2 && max != 0))) {
        awdlGetterIn(index, n, p, max);
        if (max != 0)
            awdlPublish();
    }
    if (answering && data != NULL && (set || max != 0))
        ret = answer(index, (void *)data, ret);
    if (n <= 4 || (n & 63) == 0) {
        hex[0] = '\0';
        if (p != NULL)
            for (i = 0; i < dumpBytes && i < 64; i++)
                len += snprintf(hex + len, sizeof(hex) - len, "%02x ", p[i]);
        awdlLog("airport: awdl0 %s #%u -> %s | %s\n", awdlHookNames[index], n,
                ret == kIOReturnSuccess ? (set ? "accepted" : "answered") : "unsupported",
                p != NULL ? hex : "(null)");
        awdlPublish();
    }
    return ret;
}

/*
 * The getters macOS asks first (SSID, BSSID, SYNC_ENABLED, CHANNEL_SEQUENCE, and SYNC_STATE once the engine has
 * set one), answered from what the engine set (or a neutral value) and stored only through awdlPut(), which cuts
 * an answer at the family's request size; and the setters that act on what the engine programs, which hand its
 * frames, clock and peers to the Wi-Fi kext through the bridge. Everything else keeps the default.
 */
IOReturn AirportWCN7850AWDLInterface::answer(int index, void *data, IOReturn unanswered)
{
    switch (index) {
    case 0: {	/* getSSID: AWDL has none */
        const uint32_t out[2] = { APPLE80211_VERSION, 0 };	/* version, ssid_len */

        awdlPut(index, hookCalls[index], data, out, sizeof(out));
        return kIOReturnSuccess;
    }
    case 2: {	/* getBSSID */
        uint8_t out[sizeof(uint32_t) + sizeof(awdlBssid)];
        const uint32_t ver = APPLE80211_VERSION;	/* not "version": that is a kernel global (libkern/version.h) */

        memcpy(out, &ver, sizeof(ver));
        memcpy(out + sizeof(ver), awdlBssid, sizeof(awdlBssid));
        awdlPut(index, hookCalls[index], data, out, sizeof(out));
        return kIOReturnSuccess;
    }
    case 5: {	/* getAWDL_SYNC_ENABLED */
        struct apple80211_awdl_sync_enabled out;

        out.version = APPLE80211_VERSION;
        out.unk1 = syncUnk1;
        out.enabled = syncEnabled;
        awdlPut(index, hookCalls[index], data, &out, sizeof(out));
        return kIOReturnSuccess;
    }
    case 9:	/* getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE: only once macOS has set one */
        if (!haveChannelSequence)
            return unanswered;
        awdlPut(index, hookCalls[index], data, &channelSequence, sizeof(channelSequence));
        return kIOReturnSuccess;
    case 30: {	/* setAWDL_SYNC_ENABLED: every call, it is the engine's on/off */
        const struct apple80211_awdl_sync_enabled *d = (const struct apple80211_awdl_sync_enabled *)data;
        syncUnk1 = d->unk1;
        syncEnabled = d->enabled;
        awdlLog("airport: awdl0 sync %s (unk1 %u)\n", syncEnabled ? "ENABLED" : "disabled", syncUnk1);
        awdlPublish();
        return kIOReturnSuccess;
    }
    case 35: {	/* setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE */
        const uint8_t *s;
        char hex[3 * 48 + 1];
        size_t i, len = 0;

        memcpy(&channelSequence, data, sizeof(channelSequence));
        haveChannelSequence = true;
        s = (const uint8_t *)&channelSequence.seqs;
        if (awdlBridge.version >= 4 && awdlBridge.chanseq != NULL) {
            /* 16 entries of 12 bytes from offset 16: channel u32, flags u32, u32. */
            uint32_t ch[16], fl[16];
            for (i = 0; i < 16; i++) {
                ch[i] = *(const uint32_t *)(s + 12 * i);
                fl[i] = *(const uint32_t *)(s + 12 * i + 4);
            }
            awdlBridge.chanseq(awdlBridge.ctx, ch, fl);
        }
        for (i = 0; i < 48; i++)
            len += snprintf(hex + len, sizeof(hex) - len, "%02x ", s[i]);
        awdlLog("airport: awdl0 channel sequence: length %u encoding %u steps %u duplicates %u fill %u | %s\n",
                channelSequence.length, channelSequence.encoding, channelSequence.step_count,
                channelSequence.duplicate_count, channelSequence.fill_channel, hex);
        awdlPublish();
        return kIOReturnSuccess;
    }
    case 41:	/* setAWDL_OOB_REQUEST: one frame */
    case 50:	/* setAWDL_OOB_AUTO_REQUEST: one frame, resent until the next */
        oobTx((const uint8_t *)data, index == 50);
        return kIOReturnSuccess;
    case 42: {	/* setAWDL_SYNC_FRAME_TEMPLATE: our PSF, sent while we are master */
        const uint8_t *s = (const uint8_t *)data;

        psfTemplate(*(const uint8_t *const *)(s + 8), *(const uint32_t *)(s + 4));
        return kIOReturnSuccess;
    }
    case 37:	/* setAWDL_SYNC_STATE: who we follow (the master's address at 8) */
        memcpy(syncState, data, sizeof(syncState));
        haveSyncState = true;
        syncStateSet((const uint8_t *)data + 8);
        return kIOReturnSuccess;
    case 12:	/* getAWDL_SYNC_STATE: only once the engine has set one, the 16 bytes it set, as in M0.1 */
        if (!haveSyncState)
            return unanswered;
        awdlPut(index, hookCalls[index], data, syncState, sizeof(syncState));
        return kIOReturnSuccess;
    /*
     * getPEER_CACHE_MAXIMUM_SIZE (10) is not answered, nor is any getter that awdlGetterAnswered[] does not list.
     *
     * 12 is answered as M0.1 did, so that switch 0 changes no hook answer: the family's request is 24 bytes
     * (include/Airport/apple80211_ioctl.h declares 8; the engine's setter carries the master's address at 8) and
     * the 16 bytes fit. A decline would make IO80211AWDLPeerManager::getAwdlSyncState fill the buffer itself from
     * the engine's own state, which is more truthful (the echo's word at 4 was 1 in all 14 logged sets) but is a
     * change of an answer, not a clamp. Only the API user client's handler reaches it [KC]; M0 never asked.
     *
     * 10: the family's request is 8 bytes (version, max_peers) and the engine's own call
     * (IO80211P2PDataPathManager::getCardCapableMaxPeerCacheSize) keeps them in an 8-byte local with its
     * stack canary right behind: in our hook dumps every 8-byte buffer is followed by the same eight bytes
     * (29 00 bb d7 17 a5 ea fd on 2026-11-05). The answer of awdldata and awdldata2 (2026-11-04) stored 16
     * bytes, so it overwrote the canary; that, not the Skywalk flow queues this comment used to blame, is
     * the better explanation of the two freezes at link-up. Unanswered, the engine logs "Unable to get max
     * peer cache size" and goes on without it (M0 ran like that); answering gains nothing. The Wi-Fi kext
     * gives the firmware its peers from the frames it receives instead (awdlRxGated).
     */
    case 40: {	/* setPEER_CACHE_CONTROL: operation at 4 (0 add, 2 delete, 6 update), the peer's address at 8 (Broadcom's reads) */
        static uint32_t peerOps, peerErrs;
        const uint8_t *d = (const uint8_t *)data;
        uint32_t op = *(const uint32_t *)(d + 4);
        int ret = ENXIO;
        bool logIt;

        /* The op goes on with the add flag as QWZ_AWDL_PEER_OP + op; the Wi-Fi kext decides what it does (debug.qwz.awdlpeers bit 2): this kext cannot read a sysctl. */
        if (awdlBridge.version >= 5 && awdlBridge.peer != NULL)
            ret = awdlBridge.peer(awdlBridge.ctx, d + 8, QWZ_AWDL_PEER_OP + (op < 0xff ? op : 0xff));
        /* The first 64 calls and every 64th; failures (ENXIO when stopped, ENOSPC on a full queue) have a count of their own: the first 64 and every 64th. */
        if (ret != 0)
            peerErrs++;
        logIt = ++peerOps <= 64 || (peerOps & 63) == 0 || (ret != 0 && (peerErrs <= 64 || (peerErrs & 63) == 0));
        if (!logIt)
            return kIOReturnSuccess;	/* nothing new in the ring: no republish either */
        awdlLog("airport: awdl0 peer cache control op %u for %02x:%02x:%02x:%02x:%02x:%02x (%s) -> bridge %d\n",
                op, d[8], d[9], d[10], d[11], d[12], d[13],
                op == 0 ? "add" : op == 2 ? "delete" : op == 6 ? "update" : "other", ret);
        awdlPublish();
        return kIOReturnSuccess;
    }
    case 29:	/* setAWDL_SYNC_PARAMS */
    case 31:	/* setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS */
    case 36:	/* setAWDL_MASTER_CHANNEL */
    case 39:	/* setAWDL_PRESENCE_MODE */
    case 45:	/* setAWDL_SECONDARY_MASTER_CHANNEL */
    case 46:	/* setAWDL_AF_TX_MODE */
        awdlSetterOut(index, (const uint8_t *)data);
        return unanswered;	/* M2a WP5: accepted exactly as before; the Wi-Fi kext reads the values */
    default:
        return unanswered;
    }
}

/*
 * The AWDL fixed header (category 127, OUI 00:17:f2, type 8, version 1.0,
 * subtype, reserved, phy_tx_time, target_tx_time). The engine's bodies are
 * the TLVs only (26.7.1: an OOB request starts with TLV 0x18, election
 * parameters v2): Broadcom's firmware adds the header.
 */
#define AWDL_FIXED_HDR		16	/* 8 bytes, then phy_tx_time and target_tx_time (the Wi-Fi kext stamps both) */
#define AWDL_VERSION_1_0	0x10
#define AWDL_SUBTYPE_PSF	0	/* periodic synchronization frame: the master's */
#define AWDL_SUBTYPE_MIF	3	/* master indication frame: what peers send all the time */
#define AWDL_TLV_ELECTION_V2	0x18	/* first 6 bytes: the top master's address */
#define APPLE80211_M_AWDL_SYNC_STATE_CHANGED_ID	46
#define AWDL_SYNC_MASTER	1	/* Broadcom: handleMasterEvent */
#define AWDL_SYNC_SLAVE		2	/* handleSlaveEvent */

static const uint8_t awdlBroadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/*
 * An AWDL action frame in frame (AWDL_FRAME_MAX bytes): 802.11 header
 * (destination, src, BSSID), the AWDL fixed header with subtype (its two
 * times zero until the Wi-Fi kext stamps them), then the TLVs. Its length.
 */
static size_t awdlBuildFrame(uint8_t *frame, const uint8_t *dest, const uint8_t *src, const uint8_t *bssid,
                             uint8_t subtype, const uint8_t *tlvs, size_t len)
{
    const uint8_t hdr[8] = { 0x7f, 0x00, 0x17, 0xf2, 0x08, AWDL_VERSION_1_0, subtype, 0x00 };

    memset(frame, 0, 24 + AWDL_FIXED_HDR);
    frame[0] = 0xd0;	/* management, action */
    memcpy(frame + 4, dest, 6);
    memcpy(frame + 10, src, 6);
    memcpy(frame + 16, bssid, 6);
    memcpy(frame + 24, hdr, sizeof(hdr));
    memcpy(frame + 24 + AWDL_FIXED_HDR, tlvs, len);
    return 24 + AWDL_FIXED_HDR + len;
}

/* The value of the first TLV of type in an AWDL TLV list (type, length (LE16), value), or NULL. */
static const uint8_t *awdlTlvFind(const uint8_t *tlvs, size_t len, uint8_t type, size_t *vlen)
{
    size_t off = 0, l;

    while (off + 3 <= len) {
        l = tlvs[off + 1] | (tlvs[off + 2] << 8);
        if (off + 3 + l > len)
            return NULL;
        if (tlvs[off] == type) {
            *vlen = l;
            return tlvs + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

/*
 * The engine's PSF template: TLVs, with the top master's address first in
 * election parameters v2. While that is us, the Wi-Fi kext resends it as a
 * PSF every 110 TU, as Broadcom's firmware does for a master; when it is a
 * peer, we only follow (their PSFs reach the engine) and stop ours.
 */
void AirportWCN7850AWDLInterface::psfTemplate(const uint8_t *body, uint32_t len)
{
    uint8_t frame[AWDL_FRAME_MAX];
    const uint8_t *master;
    size_t vlen = 0;
    bool weAreMaster;
    int ret = ENXIO;

    if (!awdlKernelPointer(body) || len < 3 || len > AWDL_FRAME_MAX - 24 - AWDL_FIXED_HDR ||
        awdlBridge.version < 2 || awdlBridge.tx == NULL)
        return;
    master = awdlTlvFind(body, len, AWDL_TLV_ELECTION_V2, &vlen);
    weAreMaster = master != NULL && vlen >= 6 && memcmp(master, awdlAddress, 6) == 0;
    if (weAreMaster) {
        ret = awdlBridge.tx(awdlBridge.ctx, frame,
                            awdlBuildFrame(frame, awdlBroadcast, awdlAddress, awdlBssid, AWDL_SUBTYPE_PSF, body, len), 2);
        psfActive = ret == 0;
    } else if (psfActive) {
        awdlBridge.tx(awdlBridge.ctx, NULL, 0, 2);
        psfActive = false;
    }
    if (!weAreMaster)	/* M2a WP5 (awdlclk 16): the same PSF as a follower's, in repeat slot 3; the Wi-Fi kext sends it every 440 TU only with that switch on and drops it otherwise */
        awdlBridge.tx(awdlBridge.ctx, frame,
                      awdlBuildFrame(frame, awdlBroadcast, awdlAddress, awdlBssid, AWDL_SUBTYPE_PSF, body, len),
                      QWZ_AWDL_TX_FOLLOWER_PSF);
    if (++psfLogged <= 16)
        awdlLog("airport: awdl0 PSF template: %u bytes, top master %02x:%02x:%02x:%02x:%02x:%02x -> %s (%d)\n",
                len, master && vlen >= 6 ? master[0] : 0, master && vlen >= 6 ? master[1] : 0,
                master && vlen >= 6 ? master[2] : 0, master && vlen >= 6 ? master[3] : 0,
                master && vlen >= 6 ? master[4] : 0, master && vlen >= 6 ? master[5] : 0,
                weAreMaster ? "we are master: PSF every 110 TU" : "a peer is master: no PSF of ours", ret);
}

/*
 * setAWDL_SYNC_STATE: the engine programs whom to follow (Broadcom's
 * programChipWithNewMaster). The firmware answers with
 * APPLE80211_M_AWDL_SYNC_STATE_CHANGED once it does: slave (2) for a peer,
 * master (1) for us. Without that answer the engine drops the peer and
 * goes back to its own cluster, where no peer looks at it. Posted from the
 * workloop 10 ms later, as a firmware event would be.
 */
void AirportWCN7850AWDLInterface::syncStateSet(const uint8_t *master)
{
    uint32_t state = memcmp(master, awdlAddress, 6) == 0 ? AWDL_SYNC_MASTER : AWDL_SYNC_SLAVE;
    bool newMaster = memcmp(master, syncMaster, 6) != 0;

    memcpy(syncMaster, master, 6);
    if (awdlBridge.version >= 3 && awdlBridge.master != NULL)
        awdlBridge.master(awdlBridge.ctx, master, state == AWDL_SYNC_MASTER);
    if (state == AWDL_SYNC_SLAVE && psfActive && awdlBridge.tx != NULL) {
        awdlBridge.tx(awdlBridge.ctx, NULL, 0, 2);	/* a peer is master: our PSF stops */
        psfActive = false;
    }
    if ((state != syncPosted || newMaster) && syncTimer != NULL) {
        syncPending = state;
        syncTimer->setTimeoutMS(10);
    }
}

void AirportWCN7850AWDLInterface::syncTimerFired(OSObject *owner, IOTimerEventSource *)
{
    AirportWCN7850AWDLInterface *self = OSDynamicCast(AirportWCN7850AWDLInterface, owner);
    uint32_t msg[4] = { 0, 0, 0, 0 };
    IOReturn ret;

    if (self == NULL || self->syncPending == 0 || self->awdlController == NULL)
        return;
    msg[0] = self->syncPending;
    ret = self->awdlController->postMessage(self, APPLE80211_M_AWDL_SYNC_STATE_CHANGED_ID, msg, sizeof(msg), true);
    awdlLog("airport: awdl0 sync state changed -> %s of %02x:%02x:%02x:%02x:%02x:%02x (posted 0x%x)\n",
            self->syncPending == AWDL_SYNC_MASTER ? "MASTER" : "SLAVE",
            self->syncMaster[0], self->syncMaster[1], self->syncMaster[2],
            self->syncMaster[3], self->syncMaster[4], self->syncMaster[5], ret);
    awdlPublish();
    self->syncPosted = self->syncPending;
    self->syncPending = 0;
}

/*
 * The engine's out-of-band action frame: an 802.11 action frame header
 * (destination, our address, the BSSID), the AWDL fixed header if the body
 * has none, then the body, to the Wi-Fi kext's TX queue (which stamps the
 * two times).
 */
void AirportWCN7850AWDLInterface::oobTx(const uint8_t *request, bool repeat)
{
    uint32_t len = *(const uint32_t *)(request + OOB_LEN);
    const uint8_t *body = *(const uint8_t *const *)(request + OOB_BODY);
    uint8_t frame[AWDL_FRAME_MAX];
    char hex[3 * 24 + 1];
    size_t hdr;
    bool fixed;
    int ret = ENXIO;

    if (!awdlKernelPointer(body) || len < 3 || len > AWDL_FRAME_MAX - 24 - AWDL_FIXED_HDR) {
        awdlLog("airport: awdl0 OOB request: bad body %p, %u bytes: not sent\n", body, len);
        awdlPublish();
        return;
    }
    /* A body that already starts with the AWDL fixed header goes as it is. */
    fixed = len >= AWDL_FIXED_HDR && body[0] == 0x7f && body[1] == 0x00 && body[2] == 0x17 &&
        body[3] == 0xf2 && body[4] == 0x08;
    memset(frame, 0, 24 + AWDL_FIXED_HDR);
    frame[0] = 0xd0;	/* management, action */
    memcpy(frame + 4, request + OOB_DEST, 6);
    memcpy(frame + 10, awdlAddress, 6);
    memcpy(frame + 16, request + OOB_BSSID, 6);
    hdr = 24;
    if (!fixed) {
        hdr = awdlBuildFrame(frame, request + OOB_DEST, awdlAddress, request + OOB_BSSID, AWDL_SUBTYPE_MIF,
                             body, len) - len;
    } else {
        memcpy(frame + hdr, body, len);
    }
    if (awdlBridge.version >= 2 && awdlBridge.tx != NULL)
        ret = awdlBridge.tx(awdlBridge.ctx, frame, hdr + len, repeat ? 1 : 0);
    awdlOobBody(body, len, repeat);	/* M2a (WP3, I5): a line whenever the engine's body changes, past the 64-request cap of the line below */
    if (++oobRequests <= 64 || ret != 0) {
        awdlHex(hex, sizeof(hex), body, len < 24 ? len : 24);
        awdlLog("airport: awdl0 OOB %srequest: ch %u, %u bytes%s -> %s (%d) | %s\n",
                repeat ? "auto " : "", *(const uint16_t *)(request + OOB_CHANNEL), len,
                fixed ? "" : ", sent as a MIF", ret == 0 ? "queued" : "not queued", ret, hex);
        awdlPublish();
    }
}
