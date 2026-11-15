# WCN7850-macOS

Wi-Fi, Bluetooth and AirDrop for the **Qualcomm WCN7850** (FastConnect 7800, PCI `17cb:1107`, Bluetooth USB `0489:e10a`) on a hackintosh running macOS Tahoe 26.

Two drivers live here, each in its own folder with its own licence:

| Folder | Builds | What it does |
|---|---|---|
| [`wifi/`](wifi/) | `AirportWCN7850.kext` | Native Wi-Fi through Apple's IO80211 family, with an `awdl0` interface for AirDrop |
| [`bluetooth/`](bluetooth/) | `BluetoothWCN7850.kext`, `BlueToolFixup.kext`, `IntelBTPatcher.kext` | Firmware loader for the Bluetooth half of the chip, plus the two companion kexts with this project's changes |

All three Bluetooth kexts are needed together. BlueToolFixup and IntelBTPatcher are not the stock builds: they are built from pinned upstream sources plus the patches in [`bluetooth/patches/`](bluetooth/patches/) and [`bluetooth/compat/`](bluetooth/compat/).

**Download:** the built kexts are on the [Releases page](https://github.com/Marc051297/WCN7850-macOS/releases). What changed in each release is in [CHANGELOG.md](CHANGELOG.md).

This is experimental software for one chip, developed and tested on one machine. Read [Status](#status) and [Known limits](#known-limits) before installing.

## Status

Tested on the machine described under [Tested configuration](#tested-configuration), macOS 26.7.3 (25G342), on 2026-11-15.

| Feature | Status | Notes |
|---|---|---|
| Wi-Fi: scan, join, WPA2-Personal, 802.11ac (80 MHz, 2 streams) | Works | Joins take about 0.25 s |
| WPA3 transition networks | Works | Joined on the WPA2 side |
| Signal, noise and rate in macOS | Works | Noise is measured on the home channel |
| Wi-Fi after sleep and wake, Wi-Fi off and on | Works | The firmware is reloaded on wake. Bluetooth takes longer to come back |
| Putting the Mac to sleep | Problem; fix awaiting hardware test | A connected Bluetooth LE mouse cancels sleep; [experimental.9 candidate](bluetooth/docs/experimental-9-sleep.md) |
| AirDrop, both directions (iPhone and Mac) | Works | Receive about 670 kB/s, send about 255 kB/s |
| Personal Hotspot and Instant Hotspot, 2.4 and 5 GHz | Works | Needs all three Bluetooth kexts and `-qcaadv` |
| Bluetooth: pairing, audio (A2DP), LE mice, keyboards and game controllers | Works | LE devices need `-btlfxlemask` |
| Handoff | Reported working | By the maintainer on the test machine; not measured |
| Universal Clipboard, Universal Control, Sidecar | Not tested | |
| Wi-Fi 6 (802.11ax) | Built, untested | Off unless `itlwm_he=1` |
| WPA3-only (SAE) networks | Does not work | `wifi/sae/` is unfinished |
| Continuity Camera | Does not work | Wireless: the iPhone is not offered. USB: the preview stays black |
| Apple Watch unlock | Does not work | Needs secure ranging, which is not implemented |
| iPhone Mirroring | Not possible | Needs Apple security hardware |
| 6 GHz, Wi-Fi 7, MLO | Not implemented | |
| Headset microphones | Telephone quality | macOS enables wideband speech only for Apple and Broadcom controllers |

## Tested configuration

- MSI MAG X870E Tomahawk WiFi (onboard WCN7850), Ryzen 7 9700X
- macOS Tahoe 26.7.2 (25G332) and 26.7.3 (25G342), OpenCore 1.0.8, SMBIOS MacPro7,1
- Lilu 1.7.3

AirDrop and the Bluetooth advertising path check the macOS build before they start. They run on 25G332 and 25G342 (AirDrop also on 25G241); on any other build they stay off and plain Wi-Fi and Bluetooth still work.

## Install

The short version; the details and the reasons are in [docs/install.md](docs/install.md).

1. Download the four kexts from the [latest release](https://github.com/Marc051297/WCN7850-macOS/releases), or build them (see [Building](#building)), and copy them to `EFI/OC/Kexts`.
2. In `Kernel → Add`, after `Lilu.kext`: `BlueToolFixup.kext`, `IntelBTPatcher.kext`, `BluetoothWCN7850.kext`, `AirportWCN7850.kext`.
3. Add the boot arguments below.
4. Set the two Bluetooth NVRAM values and the Wi-Fi device property from the install guide.
5. Map your USB ports: on the tested board the Bluetooth controller sits on port 17 of the chipset USB controller, beyond macOS's 15-port limit.
6. Reboot.

Do not load these next to itlwm, AirportItlwm, BTLEAdvFixup, IntelBTUnified or the stock BlueToolFixup and IntelBTPatcher.

## Boot arguments

The tested machine boots with all of these:

```
itlwm_cc=US -qwzawdlnative -itlawdlnativeup -itlawdlcoexdata -itlawdlmitx -itlawdlelicit -btlfxboardid -btlfxlemask -qcaadv
```

### Bluetooth

| Argument | Needed for | What it does |
|---|---|---|
| `-btlfxboardid` | Bluetooth at all | Makes `bluetoothd` start a third-party controller on a MacPro7,1 board id |
| `-btlfxlemask` | LE mice, keyboards, game controllers | Widens the LE event mask; without it LE devices connect but never pair |
| `-qcaadv` | AirDrop discovery by iPhones, Handoff, Instant Hotspot | Turns on LE advertising and the Qualcomm HCI handling in BlueToolFixup and IntelBTPatcher. Only on 25G332 and 25G342 |

Off switches: `-qcaadvoff` (advertising path), `-qcarpaoff` (peer-address handling), `-qcabtoff` (the firmware loader).

The local experimental.9 sleep candidate adds `-qcabsleepoff` for both new sleep
policies, `-qcabhidoff` for the HID guard and `-qcabidleoff` for the passive
advertisement assertion policy. See its [test notes](bluetooth/docs/experimental-9-sleep.md).

**Boot once without `-qcaadv` before every macOS update.** With it, reads of `bluetoothd` see patched pages, which can upset the updater's integrity checks.

### Wi-Fi and AirDrop

| Argument | What it does |
|---|---|
| `itlwm_cc=US` | Country code, passed to the firmware. Use your own country. AirDrop uses channel 149, which macOS does not allow for every country |
| `-qwzawdlnative` | Master switch: creates the `awdl0` interface inside the Wi-Fi kext |
| `-itlawdlnativeup` | Lets `awdl0` accept macOS's AWDL requests |
| `-itlawdlcoexdata` | AirDrop data while the Mac stays on its Wi-Fi network |
| `-itlawdlmitx` | Transmits the AWDL master indication frames macOS prepares |
| `-itlawdlelicit` | Adds the peer fields to AWDL sync frames so that other devices answer |

Without the five AirDrop arguments the kext is a plain Wi-Fi driver. The full list, including the runtime `debug.qwz.*` switches, is in [docs/boot-arguments.md](docs/boot-arguments.md).

## Known limits

- **One machine.** Everything above was tested on one board. Other WCN785x devices share the firmware but are untested.
- **Tied to macOS builds.** AirDrop depends on the layout of Apple's AWDL classes, and the Bluetooth advertising path patches `bluetoothd` in memory. Both are checked per build and must be re-verified for every macOS update.
- **Sleep with a Bluetooth LE mouse.** On the test machine on 2026-11-15, with a Bluetooth LE mouse connected, macOS cancelled sleep about a second after it began (the Bluetooth daemon reported the mouse as user activity), and Bluetooth kept the Mac from sleeping on its own when idle. Not fixed yet. The Wi-Fi driver itself recovers after a wake.
- **AirDrop sending is slower than receiving.** Outgoing data goes one frame at a time through the firmware's management path.
- **The `bluetoothd` patches are process-wide.** Use the WCN7850 as the only Bluetooth controller.
- **Not tested with AMFI on.** The test machine relaxes AMFI for its graphics driver.
- **`QwzMac.kext` does not build.** The tree's second Wi-Fi product (an Ethernet-style interface) is not maintained here; build with `PRODUCT=AirportWCN7850`.

## Building

On a Mac with Xcode, the Command Line Tools, git, curl and python3:

```bash
cd wifi && scripts/fetch-firmware.sh && PRODUCT=AirportWCN7850 ./build.sh
cd ../bluetooth && bash scripts/build-continuity.sh
```

The results are `wifi/build/AirportWCN7850.kext` and the three Bluetooth kexts under `bluetooth/build/`. Qualcomm's firmware is downloaded from linux-firmware at a pinned release and checked against `firmware/SHA256SUMS`; it is not in this repository.

Tests: `for t in wifi/tests/test_*.py; do python3 "$t"; done`, `python3 wifi/tools/test_awdl.py`, and `bash bluetooth/scripts/test-continuity.sh`.

## History

The history of both upstream projects is kept, with every commit moved under `wifi/` or `bluetooth/` and a trailer naming the original commit. Apart from that move the imported commits are unchanged, except that the maintainer's own commits carry one GitHub identity. Work between 2026-11-04 and 2026-11-15 was not committed at the time; those commits were rebuilt from dated source backups, one per test build, and say so.

## Licences and credits

Each folder keeps its own licence files; [LICENSES.md](LICENSES.md) has the overview.

- **[razvanccr](https://github.com/razvanccr)**: AirportWCN7850 and BluetoothWCN7850, which this project continues, and the monitor-mode AirDrop cell.
- **OpenBSD** `qwz(4)` (Stefan Sperling and the OpenBSD developers) and Linux **ath12k**: the WCN7850 driver core.
- **[itlwm](https://github.com/OpenIntelWireless/itlwm)** by zxystd and the AirportItlwm layer by laobamac: the 802.11 stack and the Apple-facing interface.
- **[ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm)** by retropineaple: the native AWDL interface this driver's `awdl0` is ported from, and the advertising patch signatures of BTLEAdvFixup.
- **[BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM)** (BlueToolFixup) and **[IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware)** (IntelBTPatcher): the two companion kexts.
- **[Lilu](https://github.com/acidanthera/Lilu)** and **[MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)** by acidanthera.
- **Qualcomm**: the WCN7850 firmware, redistributable in binary form under `LICENSE.QualcommAtheros_ath10k`.

Parts of this work were written with Claude (Anthropic) and Codex (OpenAI).
