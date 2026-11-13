//
//  QwzAWDLRadio.cpp
//  AirportWCN7850
//
//  The radio side of the native AWDL port, slice F1: only the three policy answers the Apple half asks the radio for.
//  On Intel these live in ItlIwx.cpp beside the listener context; here they will read the WCN7850 lane's state in F2
//  (docs/forbiddenitlwm-port-map-2026-11-13.md, R1-R7). Until then: channel 149, 20 MHz class, one stream.
//
//  Copyright (c) 2026. GPL-2.0-or-later (see LICENSE).
//
#include <ItlAWDLTargets.hpp>
#include <stdint.h>
#include <pexpert/pexpert.h>

/* The channel our sync frames name and the data path uses. F2: the station's channel in same-channel mode. */
unsigned itlAWDLFixedChannel()
{
	return 149;
}

/* 128 = "80 MHz on 149" in our channel-sequence TLV (as the phone's), 0 = 20 MHz. F2: the lane's real width. */
unsigned itlAWDLChannelClass()
{
	return 0;
}
