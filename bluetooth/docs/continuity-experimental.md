# WCN7850 Continuity experiment, 1.1.0-experimental.6

Experimental.6 adds **LE Enhanced Connection Complete** (event-mask bit 9) in
the existing Qualcomm-scoped HCI hook. Extended Create Connection (0x2043),
already enabled by our userspace path, completes through this event. The old
expanded mask omitted it. Requested bits are preserved, and the real sync and
async code paths are tested. This event selection agrees with
[Linux v6.12 hci_sync.c](https://github.com/torvalds/linux/blob/v6.12/net/bluetooth/hci_sync.c#L3942).
It does not synthesize connection events or change peer addresses/keys.

Only IntelBTPatcher changes from experimental.5; the working BlueToolFixup and
BluetoothWCN7850 bundles can stay installed. The combined Wi-Fi f2m/BT exp6
handoff stages these two changed existing kexts. Native Instant Hotspot attempts
have failed at the BLE control connection before Wi-Fi scan/join (sharingd
timeout -71143, bluetoothd status 702); a successful phone join still needs a
hardware test. A separate address-resolution issue remains under investigation.
No additional address-resolution code-page patch is included in this revision.
Continuity Camera video negotiation and secure Watch ranging remain unresolved.

This updates the three kexts already in our package: BluetoothWCN7850,
BlueToolFixup and IntelBTPatcher. There is no fourth kext. Lilu remains required.
It is a build for hardware testing, not a claim of working AirDrop or Handoff.

This build enables the new path on **Tahoe 26.7.2 (25G332) and 26.7.3 (25G342)**,
with the tested Bluetooth USB device **0489:e10a**. The advertising signatures
were checked offline against the codesign-valid pristine x86_64 bluetoothd
(SHA-256 `7fd7a1a05ed9c5d8014bd890ed324136f702afb62f8bf11ae457dcc74b755b3f`).
The 25G342 x86_64 bluetoothd was also obtained from the target machine and
verified against Apple's existing code signature after reversing our known LE
mask patch in the local copy only (SHA-256
`5ac2b687fc7781a9c0b22a0e6443ed6ca6f8a6bc5bd0d0c5306ede2e6fa7d1d9`).
Every required signature occurs once and fits inside a 4 KiB page on both builds.
The runtime guard also checks the exact kernel tag
`xnu-12377.161.15.701.48~2/RELEASE_X86_64`, shared by these two builds. During
early boot the macOS build buffer can be empty or unavailable; in that case this
exact kernel fingerprint permits initialization. This does not permit another
kernel on the basis of its major version. Once the macOS build is available,
page validation and HCI requests recheck it, rejecting other non-empty builds. Remove the new flag before changing macOS versions.

## Changes

- BlueToolFixup keeps the board-id and LE event-mask fixes. With `-qcaadv` it
  also enables bluetoothd's extended advertising, random-address, extended
  scanning and extended connection paths, using BTLEAdvFixup's byte signatures.
  It uses its existing validation hook. Advertising patches are gated on the
  updated IntelBTPatcher reporting a successful USB hook.
- IntelBTPatcher keeps the legacy scan-address workaround, and in the opt-in
  path replays macOS's advertising-set address after the set is created. The
  fixed address used for legacy scanning is never used for advertising. Removed
  sets, cleared sets, controller changes and resets invalidate the address cache.
- The HCI path widens the requested LE event mask to include the real Qualcomm
  pairing events and extended advertising reports. It preserves all other bits.
  There are no synthetic PHY/data-length events and no interrupt-pipe hooks.
- HCI behavior handles synchronous and asynchronous class OUT commands. For
  asynchronous requests, an owned OUT buffer and copied completion survive until
  the native transfer completes. The legacy scan address runs before its scan
  command, and an advertising-set address replay runs after the parameters
  transfer completes, before forwarding that request's original callback.
  No synchronous USB call runs from an asynchronous completion. Malformed or
  larger opaque traffic passes through; no lock is held over a USB request.

The HCI adjustments are scoped to 0489:e10a. **bluetoothd page patches are
process-wide**, so test with this as the only Bluetooth controller. No firmware
capabilities are added by a host patch. Actual advertising acceptance must be
confirmed on the controller.

## Build

On a Mac with Xcode, Command Line Tools, git, curl and python3:

```sh
bash scripts/build-continuity.sh
```

Dependencies are pinned in `dependencies.lock.json`. Existing dependency trees
at a different commit cause a build error instead of being reset. The build runs
the HCI tests with AddressSanitizer and UndefinedBehaviorSanitizer, builds all
three kexts, checks the bundles, then writes the experimental zip under `build/`.
The package includes source, patches, dependency snapshots, licenses and hashes.
The firmware remains pinned and checked against `firmware/SHA256SUMS`.

## First test on the hackintosh

1. Use a bootable USB copy of the working EFI. Keep the existing EFI and kexts
   available for rollback. Do not replace Lilu with a different build for this test.
2. Before enabling the new path, verify the target build and patch sites:

   ```sh
   sw_vers -buildVersion
   python3 -I tools/check-continuity.py /usr/sbin/bluetoothd --build 25G332
   ```

   Use `--build 25G342` on 26.7.3. The reported build must be 25G332 or
   25G342 and the corresponding checker must pass. The checker reads only. A copy already patched in the page
   cache can fail; boot without `-qcaadv` before checking it.
3. Replace the existing BluetoothWCN7850.kext, BlueToolFixup.kext and
   IntelBTPatcher.kext on the **test EFI** with this package's three bundles.
   Preserve their existing Kernel/Add entries and ordering after Lilu.
   **IntelBTPatcher stays enabled.** Do not also load BTLEAdvFixup or
   IntelBTUnified: they compete for hooks already used by these components.
4. Keep `-btlfxboardid -btlfxlemask` and the working NVRAM keys from our README.
   Add `-qcaadv`. Do not change controller addresses or NVRAM identity fields.
5. Boot the test EFI. Check ordinary pairing and audio first, including a newly
   paired mouse/keyboard and the Xbox controller. Then open Finder's AirDrop
   window to exercise advertising. Inspect:

   ```sh
   ioreg -l -w0 -d 1 | grep 'QcaContinuity'
   ```

   `QcaContinuityBuildAllowed` should be Yes, `QcaContinuityHciEnabled` and
   `QcaContinuityHciHookRouted` should be 1. After all relevant pages have been
   used, `QcaContinuityPatchMask` should be **496** (hex 0x1f0) on both 25G332
   and 25G342: five exact matches, including two connection setter sites.
   `QcaContinuityRevision` should be
   `1.1.0-experimental.5`. `QcaContinuityPageGate` and `QcaContinuityHciGate`
   explain the decision (`verified-kernel-tag` during early boot, or
   `verified-os-build-and-kernel` if the build was available). The two
   `BuildAtInit` properties record the initial build string, `<empty>`, or
   `<unresolved>`, so a rejection is no longer silent.
   `AddressSubmissions` and `MaskSubmissions` count USB submissions; they do not
   prove HCI command acceptance or radio transmission. A missing address counter
   can simply mean that no advertising set has been requested yet.
6. Confirm an LE advertiser actually starts without HCI errors, and test Handoff
   and Universal Clipboard in both directions. Record results rather than
   assuming the new paths mean the features work. AirDrop file transfers need
   a working AWDL transport in AirportWCN7850 as well; this package doesn't
   change the separate Wi-Fi work in progress. Instant Hotspot, Continuity Camera,
   Apple Watch unlock and wideband headset microphones are not implemented or
   established by this change.

If `UsbFailures` or `ConcurrentBypasses` rises, or pairing/audio regresses, save
the counters and stop testing the new path. Root properties are diagnostics,
not a continuous HCI trace; addresses and pairing keys are not logged.

## Rollback and macOS updates

Remove `-qcaadv` (or add `-qcaadvoff`) and reboot. Keep `-btlfxlemask` to retain
the earlier LE HID fix. The advertising/HCI experiment is then inactive. To
restore the exact earlier stack, boot the original EFI or restore its three
saved kexts. Do not live-unload these Lilu plugins.

**Before any macOS update, boot once without `-qcaadv`.** Like upstream
BTLEAdvFixup, page validation patches can make reads see modified bluetoothd
pages and interfere with incremental update integrity checks. The build changes
no system files, NVRAM, EFI partitions or running services.

## Validation status

HCI policy tests and x86_64 compilation/bundle validation are automated. All
five signatures for each of 25G332 and 25G342 were checked against signed
pristine target binaries.
The saved file named bluetoothd-26.7.1 was modified and did not match the expected
signatures; it was not accepted as evidence for 25G241 support. Other versions
remain disabled pending their own checks.

Experimental.5 has booted on 25G342, with bluetoothd reporting successful
advertising and receiving nearby LE advertisements (see the hardware result
below). Independent reception of this controller's advertisements, Handoff,
Universal Clipboard and AirDrop file transfers remain to be verified.

## 25G342 reboot follow-up

A read-only SSH check on 2026-11-14 confirmed all three experimental.1 kexts
were loaded (their UUIDs matched the local builds), but the running boot arguments
did not contain `-qcaadv`. The OS had also moved to 25G342, which experimental.1
disabled. Both explain why the feature did not activate; the absent boot argument
explains why there were no diagnostic properties at all. Experimental.2 adds the
verified build to the allowlist. It still requires `-qcaadv` in the boot arguments
actually used by OpenCore, and a reboot. No target EFI or NVRAM was edited by
the diagnostic/build session.

## Early-boot guard correction in experimental.3

On 25G342, experimental.2 loaded but reported BuildAllowed No and HciEnabled 0.
The boot argument and both patcher UUIDs were confirmed correct. The earlier
initialization sampled `osversion` only once; an empty/unresolved value latched
rejection for the whole boot. Experimental.2 had no telemetry to distinguish
those two cases. The saved signed 25G342 kernel collection has `_osversion` in
zero-initialized data, while its exported `version` string already contains the
exact kernel tag above; the running machine's `uname -v` confirms the same tag.
Experimental.3 uses that exact tag for early boot, checks any populated build
again at use time, and publishes the initial value and guard decision.
Tests cover both verified builds, empty/missing early values, unknown populated
builds, older kernels, similar tag prefixes, and different kernel architectures.
No target configuration was changed by this build session.

## First activation on 25G342

On 2026-11-14, experimental.3 reported BuildAllowed Yes, HciEnabled 1,
HciHookRouted 1 and PatchMask 496. Both BuildAtInit fields were `<empty>`,
confirming the early-boot timing problem. The mask was initially documented
incorrectly: both connection signatures (named 2673 and 2672 upstream) match
both pristine signed binaries once each. All five matches fit inside pages,
and the observed mask equals their combined bitmask, 0x1f0. This corrects the
checker and documentation; no additional binary change or reboot is required.
These activation results do not establish successful HCI commands or advertising.

## Diagnostic-only update in experimental.4

Ordinary paired Bluetooth devices were reported working with experimental.3.
A read-only live log check showed advertising repeatedly failing with status 718,
despite successful activation and all five page patches. There were no published
address/mask submission counters, so this does not yet locate the failing HCI
stage. Experimental.4 changes only IntelBTPatcher instrumentation; it preserves
the same USB behavior and asynchronous-request policy. Replace only
IntelBTPatcher.kext for this test; the experimental.3 BlueToolFixup and firmware
loader remain valid. Keep the current boot arguments.

After reboot, paste the full `ioreg -l -w0 -d 1 | grep QcaContinuity` output.
TargetRequests counts calls for our controller, AsyncRequests and SyncRequests
separate the transport styles, and PrepareFailures/LastPrepareResult identify
unreadable descriptors. HciCommands and the per-opcode counters show which
commands the policy actually sees. ExtendedParamsCommands and its length and
own-address-type fields distinguish unsupported parameter layouts from missing
random addresses. These diagnostics record no address bytes, keys or payloads.
Counters publish the first four events and powers of two; they are sampled
snapshots rather than exact live totals. No advertising fix is claimed by this
diagnostic update.

## Asynchronous USB correction in experimental.5

Experimental.4 measured TargetRequests 1024, AsyncRequests 1024, SyncRequests 0,
and HciCommands 0. The old asynchronous bypass was the reason no advertising
address/mask fix ran. Experimental.5 parses and handles those native async
requests. It retains the device/provider, owns each copied OUT buffer until the
last completion, and preserves the original callback's owner, parameter, status
and byte count. Successful submission promises one callback; immediate rejection
returns an error without a callback. Additional commands have bounded timeouts.
An aborted preliminary request does not enqueue the caller's command. The real
hook is compiled against a mock USB backend and tested under ASan/UBSan for
buffer lifetime, ordering, callback identity/count, initial/extra submission
errors, short/failed transfers, allocation failure, abort, inline completion and
stale state after reset. These tests do not replace a hardware advertising test.

Replace only IntelBTPatcher.kext for this update, keeping the current firmware
loader, BlueToolFixup and boot arguments. The build is staged to the hackintosh
home folder over SSH with a versioned name; local and remote binary hashes are
checked. EFI replacement and reboot use the user's terminal commands.

After reboot, AsyncRequests and HciCommands should both rise. A nonzero
AddressSubmissions shows an address replay completed over USB, and
MaskSubmissions shows a widened mask completed. AsyncFailures includes transport
failures and short transfers. LastAsyncPhase is 0 for scan address, 1 for the
original command, 2 for the advertising-set address; LastAsyncStatus,
LastAsyncBytes and LastAsyncExpected identify the last sampled failure. Still
confirm that bluetoothd reports successful advertising instead of status 718,
then test Handoff/clipboard and normal Bluetooth.

## Experimental.5 hardware result, 2026-11-14

A read-only SSH check confirmed revision 1.1.0-experimental.5, BuildAllowed Yes,
HciEnabled 1, HciHookRouted 1 and PatchMask 496. The sampled counters included
HciCommands 256, AsyncCompletions 256, AddressSubmissions 2, MaskSubmissions 1
and AsyncFailures 0. MissingBuffers, PrepareFailures, ShortReads and
MalformedCommands were also zero. The InvalidRequests counter included
36-byte commands that intentionally pass through unchanged because they exceed
the policy's 32-byte inspection buffer; it does not establish malformed traffic.

A subsequent four-minute bluetoothd log query contained 37 occurrences of
`Started advertising successfully status=0`, no `Could not start advertising`
messages, and 65 scan-completion messages. Nearby advertisements were received,
including recognition of a same-account Mac. This confirms that the observed
advertising-start failure is resolved in this test window, and LE reception is
active. The user also reported that LE appeared to be working. End-to-end
Handoff, Universal Clipboard and AirDrop transfers have not yet been confirmed.

The diagnostic snapshot and filtered log are saved outside the source package
at `../diag/bt-exp5-20261114/`. This result adds documentation only; the tested
IntelBTPatcher binary remains MD5 `f8f36bd5d0f4ae0490e6e2e1e2d788ba`.
