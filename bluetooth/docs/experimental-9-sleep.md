# Experimental.9: Bluetooth sleep guards

Hardware validation is pending. This is a local test candidate, not a published
release or a claim that all sleep and wake cases are fixed.

Only BlueToolFixup changes. Keep IntelBTPatcher experimental.6,
BluetoothWCN7850 1.1.0 and the current Wi-Fi build. Existing experimental.8
advertising, address resolution, event masks and HCI handling remain intact.

## Evidence and scope

With an LE HID mouse connected, manual sleep was followed by a Bluetooth
`UserIsActive` assertion and an invalid dark wake. With the mouse disconnected,
the same machine reached normal sleep and woke by the power button. The
unsupported vendor sleep-command errors occurred in both cases, so they have
not been established as the cause.

Separately, repeated manufacturer-type 21 advertisements refreshed a ten-second
BTStack assertion continuously while the user was idle. The native parser has
an assertion-mask test before an otherwise independent payload-processing path.

The added patches require the existing verified-page path, the exact audited
25G342 x86_64 binary signatures, the existing kernel/build gate, and observation
of HCI requests to USB `0489:e10a`. The bridges also check the native chipset.
Unknown builds, missing instances and other chipsets keep the native behavior.

## Changes

1. Before the native no-argument LE HID wake helper declares user activity,
   check the native Core instance's `fDarkSlept` flag and sleep-transition stages
   1, 3 and 4. During those states, return without issuing that wake assertion.
   Otherwise replay the original prologue and continue in the native helper.
   No HID packet, report delivery or connection callback is dropped or invented.
2. At the native manufacturer-mask test, omit the passive assertion refresh for
   type 21 only. Continue native parsing and delivery. Other manufacturer types,
   explicit scan requests, connection/session assertions and their timers remain
   native. This is an assertion-policy change; its effect on Continuity must be
   checked on hardware.

The 126-byte helper is appended to audited executable padding after the
unchanged experimental.8 peer helper. All three new sites use the existing
original-page ledger and native filesystem verification. There is no second
kernel hook, background service, synthetic HCI event, USB wake setting, firmware
wake change or NVRAM identity edit.

## Off switches and diagnostics

Existing `-qcaadv` is required. No new enabling argument is needed.

- `-qcabsleepoff`: disable both new policies, retaining experimental.8 behavior.
- `-qcabhidoff`: disable only the HID sleep guard.
- `-qcabidleoff`: disable only the manufacturer-type 21 assertion policy.

These are boot arguments; changes take effect after reboot. The installer does
not edit them. Existing `-qcaadvoff` and `-qcarpaoff` also prevent the new routes.

`ioreg -l -w0 -d 1 | grep -E 'QcaSleep|QcaContinuity(PageRevision|VerifierState|VerifyFailures|PageRecordsFull)'`

- `QcaSleepRevision`: `experimental.9`.
- `QcaSleepPatchMask`: helpers=1, HID caller=2, idle caller=4; normally 7 after
  both relevant pages have been loaded. It records installed sites, not events
  suppressed or proof that sleep succeeded.
- `QcaSleepHidRequested`, `QcaSleepIdleRequested`: policies requested at boot.
- `QcaSleepTargetSeen`: the tested USB controller has submitted real HCI requests.
- Page revision: `experimental.9-sleep-guards`; HCI revision remains
  `1.1.0-experimental.6`; existing peer patch mask remains up to 255.

## Build-machine validation

The generator checks the pristine binary digest, original Core construction and
publication, native state fields, mask configuration, parser instructions,
unique signatures, single-page bounds and relocation-free assembly. Apple
binaries and disassembly are not in this repository or test package.

Run the normal host suite with `bash scripts/test-continuity.sh`. It exercises
the actual callbacks with ASan/UBSan, helper/caller page ordering, independent
policy switches, repeated refaults, native validation failures, concurrent
filesystem verification and the one-bundle installer's rollback faults.

For instruction-level tests, install `unicorn==2.1.4` in a development virtual
environment and run `tests/qca_sleep.py` and `tests/qca_peer_address.py` using that
environment's Python. The sleep tests execute 9,936 cases over three ASLR slides,
all 256 manufacturer types, missing Core/controller instances, chipset gates,
sleep stages, register/stack preservation and native flags. The existing peer
bridge tests remain unchanged. Emulation does not establish hardware success.

## Hardware acceptance checklist

Keep private logs outside the repository. Record the local time of each attempt.

1. Install the versioned package and reboot. Confirm the revision, masks and
   successful verifier state; check existing paired devices first.
2. Connect the LE mouse, keep it still, run `pmset sleepnow`, wait at least
   30 seconds, then wake using the power button. `pmset -g log` must show normal
   sleep followed by `Wake from Normal Sleep`, without an invalid HID dark wake.
   Repeat at least three times; also test a keyboard and reconnect after wake.
3. Locally run `sudo pmset -a sleep 1`, allow the display to sleep and leave the
   machine idle. Inspect `pmset -g assertions` and `pmset -g log` afterward. Other
   applications and the display timer can independently delay idle sleep.
4. After each wake, test AirPods playback, Xbox reconnect, LE mouse/keyboard
   pairing/reconnect, `tools/pair-mouse`, iPhone AirDrop discovery, Instant
   Hotspot and Handoff. Compare with the currently working set.
5. For isolation, boot with `-qcabhidoff` or `-qcabidleoff`. The combined off
   switch restores the old policies. The package's `install.sh --rollback`
   reinstalls the checksum-verified experimental.8 BlueToolFixup only.

Do not publish until the hardware results are recorded and the user requests
publication. Then update the top-level status row and changelog with the actual
results, and make a normal GitHub release with the exact tested kexts and notes.
