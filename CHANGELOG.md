# Changelog

Releases are on the [Releases page](https://github.com/Marc051297/WCN7850-macOS/releases). Each one lists the exact kext builds it contains.

## 1.0.0 beta 1 (2026-11-15)

The first release from this repository. Tested on macOS Tahoe 26.7.3 (25G342).

| Kext | Version | Build |
|---|---|---|
| `AirportWCN7850.kext` | 0.0.2 | `f2v-20261115` |
| `BlueToolFixup.kext` | 2.7.3 | `experimental.8-extended-legacy-peer` |
| `IntelBTPatcher.kext` | 2.5.0 | `1.1.0-experimental.6` |
| `BluetoothWCN7850.kext` | 1.1.0 | |

What works on the test machine with this set:

- Wi-Fi with WPA2 and WPA3 transition networks, 802.11ac; the driver recovers after sleep and wake.
- AirDrop in both directions through a native `awdl0` interface: about 670 kB/s receiving, about 255 kB/s sending.
- Personal Hotspot and Instant Hotspot on 2.4 and 5 GHz.
- Bluetooth pairing, audio, and LE mice, keyboards and game controllers.

Changes in the last Wi-Fi builds before this release:

- **f2v:** background scans no longer interrupt a running AirDrop transfer; the firmware protects management frames only when the join agreed to it, which stopped an iPhone's 5 GHz hotspot from dropping the connection right after the key handshake; no full scan after leaving a network; AirDrop send rate 24 Mbit/s.
- **f2u:** unacknowledged AirDrop data frames are sent again; the country code reaches the firmware, which opens the 5 GHz channels for active scanning.
- **f2t:** the beacon counters macOS's link watchdog reads are kept valid, which ended a disconnect 60 seconds after every join.
- **f2s:** a join scans the channel macOS names first (joins in about 0.25 s); a failed join leaves the network cleanly instead of blocking all scans; measured noise.

Known problem in this release: with a Bluetooth LE mouse connected, macOS cancels sleep about a second after it begins, and Bluetooth keeps the Mac from sleeping on its own when idle.

The other known limits are in the [README](README.md#known-limits).
