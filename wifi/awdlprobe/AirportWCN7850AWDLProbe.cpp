//
//  AirportWCN7850AWDLProbe.cpp
//  AirportWCN7850AWDLProbe
//
//  The AWDL kext: AirportWCN7850AWDLInterface, the IO80211AWDLProtocol
//  subclass, and nothing else. The awdl0 discovery build froze the Mac
//  during boot with that class in the Wi-Fi kext; on its own here,
//  OpenCore links it into the boot and the kernel registers it cleanly
//  (2026-11-04).
//
//  It has no personality, so nothing loads it at boot. `sudo kmutil load
//  -b com.razvanccr.AirportWCN7850AWDLProbe` does: the kernel registers
//  the class, then runs qwz_awdlprobe_start. After that, debug.qwz.awdl=1
//  makes the Wi-Fi kext allocate the class by name and create awdl0
//  (AirportWCN7850AWDLBridge.h). A freeze in either costs a plain restart.
//
//  Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
//

#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

static void logClass(const char *name)
{
    const OSSymbol *sym = OSSymbol::withCString(name);
    const OSMetaClass *mc = sym ? OSMetaClass::getMetaClassWithName(sym) : NULL;
    const OSMetaClass *super = mc ? mc->getSuperClass() : NULL;

    IOLog("AWDLProbe: class %s: %s, size %u, superclass %s, instances %u\n", name,
          mc ? "registered" : "NOT registered", mc ? mc->getClassSize() : 0,
          super ? super->getClassName() : "-", mc ? mc->getInstanceCount() : 0);
    if (sym)
        sym->release();
}

/* kmod_info's _realmain (build.sh: QWZ_KMOD_REALMAIN): runs after the class is registered. */
extern "C" kern_return_t qwz_awdlprobe_start(kmod_info_t *ki, void *data)
{
    IOLog("AWDLProbe: ==== loaded by hand: AirportWCN7850AWDLInterface is registered with the kernel; nothing is created ====\n");
    logClass("AirportWCN7850AWDLInterface");
    logClass("IO80211AWDLProtocol");
    logClass("IO80211VirtualInterface");
    IOLog("AWDLProbe: sudo sysctl debug.qwz.awdl=1 now creates awdl0 from this class\n");
    return KERN_SUCCESS;
}
