# wifi/: AirportWCN7850.kext

Native Wi-Fi for the **Qualcomm WCN7850** (FastConnect 7800, PCI `17cb:1107`) on macOS Tahoe 26: the macOS Wi-Fi menu and settings, and an `awdl0` interface for AirDrop. It is a port of OpenBSD's [`qwz(4)`](https://man.openbsd.org/qwz.4) driver on the [itlwm](https://github.com/OpenIntelWireless/itlwm) 802.11 stack, started by [razvanccr](https://github.com/razvanccr/AirportWCN7850).

Status, install steps and boot arguments are in the [top-level README](../README.md). The Bluetooth half of the chip is in [`../bluetooth/`](../bluetooth/).

## Building

Command Line Tools, git, curl and python3:

```bash
scripts/fetch-firmware.sh && PRODUCT=AirportWCN7850 ./build.sh
```

`build/AirportWCN7850.kext` is the result (`DEBUG=1` for the traced build). The firmware comes from linux-firmware (pinned release, checksums in `firmware/SHA256SUMS`), is embedded in the kext, and is not committed.

Only `PRODUCT=AirportWCN7850` builds. The second product of the tree, `QwzMac.kext`, does not compile at present.

## Tests

```bash
for t in tests/test_*.py; do python3 "$t"; done
python3 tools/test_awdl.py
```

They run on the build machine and need no hardware.

## Layout

| Path | What it holds |
|---|---|
| `qwz/` | The driver core from OpenBSD `qwz(4)`, plus this project's recovery engine (`qwz_life`, `qwz_health`), the AirDrop radio (`qwz_mon`, `qwz_awdl_cell`) and the runtime switches (`qwz_sysctl.cpp`) |
| `itl80211/`, `include/`, `controller/` | The 802.11 stack and Apple headers from itlwm |
| `airport/` | The interface macOS's Wi-Fi stack talks to |
| `airport/awdlnative/` | The native `awdl0` interface, ported from ForbiddenItlwm (see its `NOTICE`) |
| `awdlprobe/` | A separate probe kext from the AirDrop bring-up; not needed for normal use |
| `firmware/`, `firmware-c7/` | Checksums and scripts for the firmware download |
| `sae/` | Unfinished WPA3-SAE work; not built |
| `scripts/`, `tools/`, `tests/` | Capture and diagnosis scripts, the ABI audit, host tests |
| `docs/`, `release/` | Upstream's notes and release notes up to 0.0.2-tahoe.5 |

The notes in `docs/` were written before the native AirDrop work. Where they say AirDrop cannot work on this chip, they are out of date.

## Licences and credits

- `qwz/qwz.c`, `qwz/if_qwz_pci.c`, `qwz/qwzreg.h`, `qwz/qwzvar.h`: OpenBSD `qwz(4)` (Stefan Sperling and the OpenBSD developers), ISC, with code derived from the Linux `ath12k` driver under the Linux Foundation / Qualcomm BSD licence.
- `itl80211/`, `include/`, `controller/` (except `ItlQwz.*`): itlwm by zxystd, GPL-2.0-or-later, with OpenBSD net80211 under its BSD/ISC licences. `airport/`: the AirportItlwm "WCL" layer by laobamac, as reshaped by [SimpleMtkWlan](https://github.com/pengubow/SimpleMtkWlan).
- `airport/awdlnative/` and the files its `NOTICE` lists: [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) by retropineaple, GPL-2.0.
- `qwz/qwz_compat.*`, `controller/ItlQwz.*`, `scripts/`: GPL-2.0-or-later.
- WCN7850 firmware: Qualcomm, redistributable in binary form under `LICENSE.QualcommAtheros_ath10k`.
- Built with [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK).

The whole kext is distributed under GPL-2.0-or-later (`LICENSE`). The revisions of the upstream files are in [`UPSTREAM.md`](UPSTREAM.md).
