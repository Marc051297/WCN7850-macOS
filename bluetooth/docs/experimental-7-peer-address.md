# Experimental.7: Bluetooth peer radio addresses, 25G342

Only **BlueToolFixup** changes in this test. Keep IntelBTPatcher experimental.6
and BluetoothWCN7850 installed, and keep the existing `-qcaadv` boot argument.
The firmware and Wi-Fi kext are unchanged. No runtime companion is added.

## Captured failure

Instant Hotspot fails during the BLE/L2CAP phone-enable exchange with
RPErrorDomain -71143, before a Wi-Fi join request. The controller accepts Filter
Accept List additions, but the connection never becomes ready. Status 702 in
the trace follows successful cancellation; it is not immediate HCI rejection.

The native third-party-controller path resolves advertisements to identity
addresses but lacks the initialized Apple privacy backend needed to connect
using those identities. This candidate preserves the verified current RPA for
the two connection/list-maintenance paths. This is a hardware-test candidate,
not an established fix for the captured phone or for manual hotspot joining.

## Implementation and scope

- Only the audited x86_64 bluetoothd on **25G342** gets the seven peer-address
  patches. The existing kernel fingerprint and Qualcomm HCI-hook gates apply.
- A legacy connectable undirected report retains its verified RPA separately
  while native identity processing continues. An internal marker is consumed
  before the report reaches cache/listener consumers. Nullable device lookups
  and unrelated native callers preserve their original behavior.
- The device's native mutex protects the radio-address update. Identity,
  pairing keys, UUIDs, authentication and real controller completions remain
  native. Only two initiating/list-maintenance call sites use the current radio
  address; the global paired predicate is not changed.
- New calls land in verified executable padding after __eh_frame. Helper
  ledger capacity is reserved before redirecting a caller. There is no on-disk
  bluetoothd modification, executable-header patch or new userspace process.
- The existing BlueToolFixup cs_validate_page hook records original/modified
  pages. A repeat code-signature check sees the original bytes. The filesystem's
  verifier receives a private block copy with only recorded modifications
  restored. Its real success/failure result is propagated; unrelated corruption
  is not hidden. A persistent vnode reference prevents cross-file reuse.
- The optional APFS verifier route checks the matching kernel instruction
  pattern, descriptor reference and vnode operations/mount/private-data fields.
  If it cannot establish the route, peer-address patches stay off and the
  existing BlueToolFixup path remains. `-qcarpaoff` disables the new path.

## Offline validation

The pristine 25G342 binary digest, unique single-page signatures, original
native call targets and executable-padding boundary are checked by
`tools/build-peer-address-patches.py`. All injected machine code must have no
unresolved object relocations. The generated header matches its assembly.

`tests/qca_peer_address.py` executes the actual x86_64 helpers with three ASLR
slides, different controllers/callers, null devices, marker cleanup and identity
retention. Development dependency: Unicorn 2.1.4; it is never installed on the
hackintosh or loaded by a kext.

ASan/UBSan tests cover the page ledger and the actual validation/verification
callbacks, including 500 untouched pages before the helper faults, repeated
validation, concurrent faults and verification, native failures and tampering.
The matching kernel collection has the exact XNU fingerprint and its real
VNOP_VERIFY decodes the vnode operations offset as 0xd0. Existing HCI/async
tests pass. The x86_64 BlueToolFixup builds and its plist validates.

The one-bundle installer is tested against a mock EFI for success, corrupt
source/copy and failed swap. It preserves config.plist and the other three
kexts, keeps a backup, and restores the original on failure.

## Hardware test

After installation/reboot, inspect the root QcaContinuity properties:

- `PageRevision`: `experimental.7-peer-address`.
- `VerifierState`: 1 means installed; 2–6 explain an unavailable/failed route.
- `RpaPatchMask`: up to 127 as all seven pages/paths are used. Lazy page faults
  can leave a partial mask immediately after login.
- `PageRejected`, `PageRecordsFull`, `VerifyFailures`: expect zero.
- `PageRestores`/`VerifyRestores` may grow when patched pages are revalidated.
- Existing `Revision` remains `1.1.0-experimental.6`: that identifies the
  unchanged IntelBTPatcher HCI implementation, not this BlueToolFixup update.

First confirm ordinary Bluetooth devices, then one Instant Hotspot attempt.
Read the BLE enable result, Wi-Fi scan/join/authentication result, listener state
and both transfer directions. A success still requires sleep/wake and repeated
connections before promotion from experimental status.

The Wi-Fi scan disappearance reported later on f2n occurred **before** this
update was installed. It remains a separate unresolved recovery regression.
This build does not claim full WPA3-SAE, Camera video or secure Watch ranging.
