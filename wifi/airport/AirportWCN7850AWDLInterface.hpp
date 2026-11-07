//
//  AirportWCN7850AWDLInterface.hpp
//  AirportWCN7850
//
//  awdl0 on Tahoe's native Wi-Fi stack. Tahoe runs AWDL itself, in
//  IO80211Family: IO80211VirtualInterface::start() creates an
//  IO80211AWDLPeerManager for an interface of role 6 (AWDL), and that
//  engine drives the chip through the 63 hooks of IO80211AWDLProtocol.
//  This class is the driver's side of those hooks, laid out after Apple's
//  AppleBCMWLANProximityInterface (see include/Airport/macOS26/
//  IO80211AWDLProtocol.h).
//
//  It lives in the AWDL kext (awdlprobe/, loaded by hand), never in the
//  Wi-Fi kext: the Wi-Fi kext allocates it by name and calls init() below
//  through IO80211VirtualInterface (AirportWCN7850AWDLBridge.h).
//
//  Every hook is logged to the QwzMacLog ring (the first four calls and every
//  64th; the first call of each getter also logs the buffer the family handed
//  us, before we touch it). Setters are accepted, and the ones that carry the
//  engine's frames, clock and peers hand them to the Wi-Fi kext through the
//  bridge. Five getters are answered (SSID, BSSID, SYNC_ENABLED,
//  CHANNEL_SEQUENCE, SYNC_STATE), each within the family's own request size;
//  every other getter answers unsupported.
//
//  Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
//

#ifndef AirportWCN7850AWDLInterface_hpp
#define AirportWCN7850AWDLInterface_hpp

#include "Airport/Apple80211.h"
#include "macOS26/IO80211AWDLProtocol.h"
#include <IOKit/IOTimerEventSource.h>

#define AWDL_HOOK_COUNT	63

class AirportWCN7850AWDLInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(AirportWCN7850AWDLInterface)

public:
    /* As AppleBCMWLANProximityInterface::init: init(), then IO80211VirtualInterface::init with role 6, interface id 2. */
    virtual bool init(IO80211Controller *controller, ether_addr *address, unsigned int role, char const *name) APPLE_KEXT_OVERRIDE;

    virtual const char *getBSDNamePrefix() APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMacAddress(ether_addr &) APPLE_KEXT_OVERRIDE;

#include "AirportWCN7850AWDLHooks.inc.h"

private:
    IOReturn hook(int index, const void *data, bool set, size_t dumpBytes);
    IOReturn answer(int index, void *data, IOReturn unanswered);
    void oobTx(const uint8_t *request, bool repeat);
    void psfTemplate(const uint8_t *body, uint32_t len);
    void syncStateSet(const uint8_t *master);
    static void syncTimerFired(OSObject *owner, IOTimerEventSource *sender);

    uint32_t oobRequests;
    /*
     * What Broadcom's firmware reports once it follows what the engine set:
     * APPLE80211_M_AWDL_SYNC_STATE_CHANGED (1 master, 2 slave), posted from
     * the controller's workloop (syncTimer).
     */
    IO80211Controller *awdlController;
    IOTimerEventSource *syncTimer;
    uint32_t syncPending;	/* the state to post; 0: none */
    uint32_t syncPosted;	/* the last state posted */
    uint8_t syncMaster[6];
    uint8_t syncState[16];	/* the last setAWDL_SYNC_STATE, for its getter */
    bool haveSyncState;
    bool psfActive;		/* our PSF is being resent: we are master */
    uint32_t psfLogged;

    uint32_t hookCalls[AWDL_HOOK_COUNT];
    uint8_t awdlAddress[6];
    bool answering;	/* the hook table's names match the indices answer() uses */
    /* What the engine last set, for its getters. */
    uint32_t syncUnk1;
    uint32_t syncEnabled;
    bool haveChannelSequence;
    struct apple80211_awdl_sync_channel_sequence channelSequence;
};

#endif /* AirportWCN7850AWDLInterface_hpp */
