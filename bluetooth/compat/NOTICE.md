# Compatibility code provenance

`QcaContinuityPatches.hpp` is copied from `BTLEAdvFixup/BTLEAdvPatches.hpp`
at ForbiddenItlwm commit `69e88fa0fdb0ef7122f2aee81111651c32260a62`
(BTLEAdvFixup 0.9.1). It retains the upstream comments and is GPL-3.0-only;
the full license is in `LICENSE.GPL-3.0`.
https://github.com/retropineaple/ForbiddenItlwm/tree/69e88fa0fdb0ef7122f2aee81111651c32260a62/BTLEAdvFixup

The patch sites are integrated into the already bundled BlueToolFixup, using
its existing validation hook. The Intel-only synthetic event code and USB pipe
hooks from BTLEAdvFixup are not included. The Qualcomm HCI policy and wrapper
were written for this project using standard HCI commands. `QcaBluetoothHci.hpp`
is MIT; `QcaBluetoothHooks.inc`, which is integrated into GPL IntelBTPatcher,
is GPL-3.0-only. The MIT firmware loader remains a separate executable.

The LE event-mask and board-id changes in `bluetoolfixup-continuity.patch`
retain the fixes from the local BluetoothWCN7850 `ble-hid-pairing` fork.
Upstream source and licenses for both existing dependencies are pinned by
`dependencies.lock.json` and included in the experimental package's source
directory. Keep their notices with any redistributed builds.
