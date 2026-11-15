# Experimental.8: standard extended reports carrying legacy advertisements

Only BlueToolFixup changes. Keep IntelBTPatcher experimental.6, the firmware
loader, Wi-Fi f2o and the existing -qcaadv flag. No companion is added.

## Reproduced source gap

Experimental.7 redirected the legacy advertising-report handler, retained the
verified RPA only for its return address and event 0, and decoded its private
marker only for normalized event 0. The standard HCI Extended Advertising
Report handler also calls the native AH resolver, but its call was untouched.
On the audited 25G342 binary it normalizes a legacy ADV_IND to event 0xb0,
retains raw event bits 0x13 at descriptor+0x1a, and leaves its non-legacy flag
at +0x2b clear. Consequently the same connectable legacy advertisement received
through this standard handler could not use experimental.7's preservation path.

This is a source-proven omission. It is not yet proof that this phone's failed
Hotspot attempt takes the affected branch.

## Change and bounds

The eighth patch redirects the standard handler's resolver call at 0x20dc6a.
An entry bridge clears only the seven private scratch bytes at +0x33 for that
exact legacy ADV_IND classification. The actual directed-address field at
+0x24, its bytes, all flags and other report types remain native. After native
AH resolution succeeds, the retain bridge recognizes the audited standard
handler's return address and the same exact classification, while preserving
the native identity copy. The decoder accepts the marker for event 0 or 0xb0
only when the native non-legacy flag is clear and the address has RPA shape.

True extended/non-legacy, directed, scan-response and Apple vendor report
handlers gain no new path. Controller and OS/kernel gates remain as before.
No identity, pairing material, successful event or authentication is synthesized.

The helper is 348 bytes and the decoder remains within its audited 90-byte
window. The generator verifies the pristine binary digest, original native
call targets, unique signatures, page bounds and lack of unresolved assembly
relocations. Original-page and filesystem-verification handling is unchanged.

## Diagnostics

- PageRevision: experimental.8-extended-legacy-peer.
- RpaPatchMask: up to 255 for all eight installed sites; this records installed
  code sites, not execution of a phone-specific branch.
- HCI Revision correctly stays 1.1.0-experimental.6.
- LastRejectedOffset/Reason/Flags explain a PageRejected increment. Reason 1:
  signature before expected file offset; 2: record failure; 3: saved-original
  mismatch; 4: native validated=false; 5: native tainted=true. Flags bit 0 is
  native validated, bit 1 native tainted. These diagnostics do not change or
  override the native verdict. The experimental.7 count of one had no reason
  information; do not infer its cause from the count alone.

## Validation and hardware status

The actual x86_64 helpers run under Unicorn at three ASLR slides. New tests
cover extended-handler entry/retention, controller scoping, raw event types,
legacy/non-legacy flags, directed-address preservation and decoder acceptance.
Existing register/stack/native-ABI, identity and nullable-device tests pass.
ASan/UBSan page/verification tests pass, including differentiated rejection
reasons. Existing HCI and asynchronous ownership tests pass. The x86_64 kext
builds, its plist validates, and imports exactly match experimental.7.
The one-bundle installer passes success, corrupt-source/copy and failed-swap
tests while preserving config and the other three bundles.

Hardware testing is pending. Check paired Bluetooth devices, Instant Hotspot,
then return to normal Wi-Fi and test outgoing AirDrop. A failed Hotspot must
be classified as BLE control, Wi-Fi scan, association, authentication or IP
configuration using fresh logs; the candidate does not establish success.

The fresh Wi-Fi f2o capture shows real scans completing in 1.3–5.1 seconds,
neighbour RSSI -86 to -28 dBm and no descriptor mismatch. One outgoing AirDrop
session times out over AWDL while the Hotspot control session times out over
BLE. They are distinct observed failures; this Bluetooth patch is not claimed
as a complete fix for both.

## First hardware result — 15 November, 06:38:11 SAST boot

Exp8 is loaded: mask 255, VerifierState 1. LastRejectedOffset=0, Reason=4,
Flags=0 identifies a native validated=false page; native failure is preserved.
No verification failure or full page ledger is reported.

Fresh hotspot-enable requests now pass BLE control. At 06:40:40.798 airportd
reports that Personal Hotspot was enabled on channel 44, then repeatedly
fails to match the hotspot Wi-Fi network and returns -3905. Rapport records
NoError for the BLE request. This is progress beyond exp7's BLE connection
timeout, not proof of a complete hotspot connection or of every Bluetooth
feature. The next blocker is Wi-Fi scan/matching before authentication.
AirDrop still has Waiting/slow-send stalls; live queues drain rather than
remaining full. Camera, Watch and Bluetooth wake recovery remain unverified.

Subsequent controlled hotspot tests succeed on the same f2o/exp8 boot: the
user confirms working internet with Maximize Compatibility enabled (channel
6), then a successful join with it disabled (channel 44). The latter selects
WPA2-PSK from the transition-mode network. The Wi-Fi scan/list probe omission
is being corrected in f2p; full SAE, Camera and Watch are still separate work.
