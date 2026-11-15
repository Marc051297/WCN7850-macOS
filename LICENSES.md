# Licences

This repository holds two separate works. They are built into separate kexts and are not linked together. Each folder carries its own licence files; this page is only the overview.

## `wifi/` (AirportWCN7850.kext)

Distributed as a whole under **GPL-2.0-or-later**: [`wifi/LICENSE`](wifi/LICENSE).

| Part | Origin | Licence |
|---|---|---|
| `qwz/qwz.c`, `qwz/if_qwz_pci.c`, `qwz/qwzreg.h`, `qwz/qwzvar.h` | OpenBSD `qwz(4)`, with code derived from Linux `ath12k` | ISC; BSD (Linux Foundation, Qualcomm) |
| `itl80211/`, `include/`, `controller/` | itlwm, with OpenBSD net80211 | GPL-2.0-or-later; BSD, ISC |
| `airport/` | The AirportItlwm layer, and this project's work | GPL-2.0-or-later |
| `airport/awdlnative/` and the files its `NOTICE` lists | ForbiddenItlwm | GPL-2.0, see [`wifi/airport/awdlnative/NOTICE`](wifi/airport/awdlnative/NOTICE) |
| Everything else | razvanccr and this project | GPL-2.0-or-later |

The revisions the upstream files were taken from are in [`wifi/UPSTREAM.md`](wifi/UPSTREAM.md).

## `bluetooth/` (BluetoothWCN7850.kext, BlueToolFixup.kext, IntelBTPatcher.kext)

Three separate executables with different licences.

| Part | Origin | Licence |
|---|---|---|
| `kext/`, `scripts/`, `tools/` (the firmware loader) | razvanccr and this project | MIT: [`bluetooth/LICENSE`](bluetooth/LICENSE) |
| `patches/bluetoolfixup-continuity.patch`, `compat/QcaPageGuard.inc`, `compat/QcaContinuityPatches.hpp` | Changes to BlueToolFixup (BrcmPatchRAM); the patch signatures are from BTLEAdvFixup (ForbiddenItlwm) | GPL-3.0-only: [`bluetooth/compat/LICENSE.GPL-3.0`](bluetooth/compat/LICENSE.GPL-3.0) |
| `patches/intelbtpatcher-continuity.patch`, `compat/QcaBluetoothHooks.inc` | Changes to IntelBTPatcher (IntelBluetoothFirmware) | GPL-3.0-only |
| `compat/QcaBluetoothHci.hpp`, `compat/QcaValidatedPages.hpp`, `compat/QcaVerifyLayout.hpp`, `compat/QcaPeerAddress25G342.S` | This project | MIT |

The provenance of every file in `compat/` is recorded in [`bluetooth/compat/NOTICE.md`](bluetooth/compat/NOTICE.md). BlueToolFixup and IntelBTPatcher themselves are not copied here: the build fetches them at the commits pinned in [`bluetooth/dependencies.lock.json`](bluetooth/dependencies.lock.json), under their own licences.

## Firmware

Qualcomm's WCN7850 firmware is not in this repository. The build scripts download it from linux-firmware at a pinned release and verify its checksums. It is redistributable in binary form under `LICENSE.QualcommAtheros_ath10k`; a built kext embeds the firmware, so that licence text must accompany any kext you pass on.

## Apple software

No Apple binaries are included. The Bluetooth changes are applied to `bluetoothd` in memory on the user's own machine and do not modify any file.

## Top-level documentation

`README.md` and `docs/` are under GPL-2.0-or-later.
