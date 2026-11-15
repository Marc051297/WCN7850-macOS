# Boot arguments and runtime switches

Boot arguments go in OpenCore's `boot-args`. Runtime switches are set with `sudo sysctl <name>=<value>` and return to their defaults at every boot.

## Bluetooth

| Argument | Read by | What it does |
|---|---|---|
| `-btlfxboardid` | BlueToolFixup | Applies the board-id fix `bluetoothd` needs to start the controller with SMBIOS MacPro7,1 on Tahoe. Required |
| `-btlfxlemask` | BlueToolFixup | Widens the LE event mask. Without it LE mice, keyboards and game controllers connect but never pair |
| `-qcaadv` | BlueToolFixup, IntelBTPatcher | Turns on LE advertising, extended scanning and connections, and the Qualcomm HCI handling. Needed for iPhones to find the Mac (AirDrop, Handoff) and for Instant Hotspot. Only active on macOS 25G332 and 25G342 |
| `-qcaadvoff` | BlueToolFixup, IntelBTPatcher | Forces the `-qcaadv` path off |
| `-qcarpaoff` | BlueToolFixup | Turns off the peer-address handling added in experimental.7 and .8 |
| `-qcabsleepoff` | BlueToolFixup experimental.9 | Disables both new sleep guards; retains experimental.8 behavior |
| `-qcabhidoff` | BlueToolFixup experimental.9 | Disables only the sleep-state guard on the LE HID wake notification |
| `-qcabidleoff` | BlueToolFixup experimental.9 | Disables only the manufacturer-type 21 passive assertion policy |
| `-qcabtoff` | BluetoothWCN7850 | Disables the firmware loader |
| `qcabt_stage=<n>` | BluetoothWCN7850 | Stops the loader before stage n, for diagnosis |

Boot once without `-qcaadv` before every macOS update.

The background, validation and diagnostic keys are in [`bluetooth/docs/continuity-experimental.md`](../bluetooth/docs/continuity-experimental.md) and the two `experimental-*.md` files beside it.

## Wi-Fi

| Argument | What it does |
|---|---|
| `itlwm_cc=<CC>` | Two-letter country code. Passed to the firmware, which then opens the channels that country allows |
| `itlwm_he=1` | Tries Wi-Fi 6 (802.11ax). Untested |
| `-novht` | Falls back to 802.11n |
| `-noht40` | 20 MHz channels only on 802.11n |
| `-qwzoff` | Disables the driver |
| `-qwznowakefix` | Restores the old wake path, without the firmware reload |
| `-qwznoradioreset` | Wi-Fi off and on stops the interface only, without a chip reset |
| `-qwznostallfix` | Turns off the restart after a data stall |
| `-qwzstallsbr` | Makes the second data-stall restart reset the PCIe link |

## AirDrop

| Argument | What it does |
|---|---|
| `-qwzawdlnative` | Master switch. Creates the `awdl0` interface inside the Wi-Fi kext. Only on macOS builds the driver has verified (25G241, 25G332, 25G342) |
| `-itlawdlnativeup` | Lets `awdl0` accept macOS's AWDL requests |
| `-itlawdlcoexdata` | Carries AirDrop data while the Mac stays on its Wi-Fi network |
| `-itlawdlmitx` | Transmits the AWDL master indication frames macOS prepares |
| `-itlawdlelicit` | Adds the peer fields to AWDL sync frames so that other devices answer |
| `-qwzawdlmanual` | Does not start the AirDrop radio automatically after a join |
| `-qwzawdlpeerpauseoff` | Sends without waiting for the receiver's listening windows |
| `-itlawdlanybuild` | Skips the macOS build check. For development only; the class layout may not match |

The tested machine uses the first five. The names starting with `-itlawdl` are kept from ForbiddenItlwm so the two trees stay comparable.

## Runtime switches worth knowing

| Switch | Default | What it does |
|---|---|---|
| `debug.qwz.publish` | | Write 1 to publish the log ring as `QwzMacLog` in the I/O Registry |
| `debug.qwz.nativescanrest` | 120 | While an AirDrop transfer runs, background scans are answered from the known-network list and a real scan runs at most once in this many seconds. 0 turns this off |
| `debug.qwz.monrate` | 24 | Send rate of the AirDrop radio in Mbit/s (6 to 54). 0 uses the firmware's own |
| `debug.qwz.nativeresend` | 6 | How often an unacknowledged AirDrop data frame is sent |
| `debug.qwz.pmfstrict` | 1 | Management frames are protected only when the join agreed to it. 0 protects them whenever the access point offers it |
| `debug.qwz.joinidle` | 1 | No full scan after leaving a network. 0 restores the scan |
| `debug.qwz.joinhint` | 1 | A join scans the channel macOS names first. 0 scans every channel |
| `debug.qwz.authfast` | 1 | An unacknowledged authentication frame is retried within a second. 0 waits 5 seconds |
| `debug.qwz.regcc` | 1 | The country code is set with the firmware's current-country command. 0 uses the init command only |

The other `debug.qwz.*` switches belong to experiments and diagnosis. Each one is described where it is declared, in [`wifi/qwz/qwz_sysctl.cpp`](../wifi/qwz/qwz_sysctl.cpp).
