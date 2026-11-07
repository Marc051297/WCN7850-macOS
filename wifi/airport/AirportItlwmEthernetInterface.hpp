//
//  AirportItlwmEthernetInterface.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmEthernetInterface_hpp
#define AirportItlwmEthernetInterface_hpp

extern "C" {
#include <net/bpf.h>
}
#include "Airport/Apple80211.h"
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <sys/kernel_types.h>
#include <IOKit/network/IOEthernetInterface.h>

class AirportItlwmEthernetInterface : public IOEthernetInterface {
    OSDeclareDefaultStructors(AirportItlwmEthernetInterface)
    
public:
    virtual IOReturn attachToDataLinkLayer( IOOptionBits options,
                                            void *       parameter ) override;
    
    virtual void     detachFromDataLinkLayer( IOOptionBits options,
                                              void *       parameter ) override;
    
    virtual bool initWithSkywalkInterfaceAndProvider(IONetworkController *controller, IO80211SkywalkInterface *interface);
    
    virtual bool setLinkState(IO80211LinkState state);
    
    static errno_t bpfOutputPacket(ifnet_t interface, u_int32_t data_link_type,
                                  mbuf_t packet);
    
    static errno_t bpfTap(ifnet_t interface, u_int32_t data_link_type,
                          bpf_tap_mode direction);
    
    virtual UInt32   inputPacket(
                                 mbuf_t          packet,
                                 UInt32          length  = 0,
                                 IOOptionBits    options = 0,
                                 void *          param   = 0 ) override;
    
    virtual IOService * getProvider( void ) const override;

    /* awdl0's BSD interface is this class too, named awdl: set before registerService(). */
    void setNamePrefix(const char *prefix) { namePrefix = prefix; }
    virtual const char * getNamePrefix() const override;
    /*
     * awdl0's own address, before registerService(): its ifnet unique id and
     * link-layer address. Without it the unique id is the controller's (en1's)
     * MAC, and the kernel refuses a second ifnet with the same id.
     */
    void setOwnAddress(const uint8_t *address);

protected:
    virtual bool initIfnetParams(struct ifnet_init_params *params) override;

private:
#ifdef AIRPORT_WCL
    static IOReturn prepareBSDGated(OSObject *, void *, void *, void *, void *);
#endif
    IO80211SkywalkInterface *interface;
    bool isAttach;
    const char *namePrefix;	/* NULL: IOEthernetInterface's "en" */
    bool hasOwnAddress;	/* false: the controller's address, as for en1 */
    uint8_t ownAddress[6];
};

#endif /* AirportItlwmEthernetInterface_hpp */
