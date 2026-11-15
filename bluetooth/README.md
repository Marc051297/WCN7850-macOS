# BluetoothWCN7850

Bluetooth for the **Qualcomm WCN7850** (FastConnect 7800) on macOS. The chip's Bluetooth half is a USB device (`0489:e10a`) that boots with only a ROM; this kext pushes Qualcomm's RAM patch and NVM into it at boot, the way Linux's `btusb` does, and macOS's own Bluetooth stack takes it from there. The Wi-Fi half is in [`../wifi/`](../wifi/); status, install steps and boot arguments for both are in the [top-level README](../README.md).

**Tested on:** MSI MAG X870 Tomahawk WiFi (onboard WCN7850), macOS 26.7.1 Tahoe, OpenCore, SMBIOS MacPro7,1. Pairing, A2DP audio and LE devices work; the controller shows up in System Information with its own address.

## What gets built

Three kexts, all needed together:

- `BluetoothWCN7850.kext`: the firmware loader in `kext/`.
- `BlueToolFixup.kext`: [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM)'s, with `patches/bluetoolfixup-continuity.patch` and the files it takes from `compat/`. It carries the board-id fix for macOS 26, the LE event mask that lets LE devices pair (`-btlfxlemask`), and the opt-in advertising path (`-qcaadv`).
- `IntelBTPatcher.kext`: [IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware)'s, with `patches/intelbtpatcher-continuity.patch`: the HCI handling for this controller.

The two companions are fetched at the commits pinned in `dependencies.lock.json`. The stock builds do not work for this controller.

The advertising path is experimental and restricted to Tahoe builds 25G332 and 25G342: see [docs/continuity-experimental.md](docs/continuity-experimental.md) and the two `experimental-*.md` notes for what it changes, how it was validated and its limits.

## Install

**Before anything else: USB port mapping.** macOS only uses the first 15 ports of each USB controller, and on this board the Bluetooth controller sits on port 17 of the chipset controller (`XH00`), so without a port map the device does not even appear in `ioreg`. Make a map with [USBToolBox](https://github.com/USBToolBox/tool) (or USBMap) that includes the Bluetooth port; check with `ioreg -p IOUSB` that a device `0489:e10a` is listed before installing anything from here.

1. `Kernel → Add`: `Lilu.kext`, then `BlueToolFixup.kext` and `IntelBTPatcher.kext` (the builds from this repository; both are Lilu plug-ins), then `BluetoothWCN7850.kext`.
2. `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`: add `-btlfxboardid -btlfxlemask` to `boot-args`, and `-qcaadv` for AirDrop discovery, Handoff and Instant Hotspot (see [boot arguments](../docs/boot-arguments.md)); `bluetoothExternalDongleFailed` = `00`; `bluetoothInternalControllerInfo` = 14 zero bytes. Put the same two keys under `NVRAM → Delete`.
3. Reboot.

Why the companions: Tahoe's `bluetoothd` boots the controller through a UART on a MacPro7,1 board-id, and only accepts Apple/Broadcom vendor ids. BlueToolFixup fixes both, but its board-id patch searched for a string macOS 26 removed, so the build here carries a fix for it (`patches/bluetoolfixup-continuity.patch`). IntelBTPatcher, despite the name, is a generic shim: without it `bluetoothd` restarts every 10 s on LE scan setup.

## What does not work, or is not tested

- Headset microphones are telephone quality (8 kHz CVSD). macOS enables wideband speech only through Apple and Broadcom vendor commands; a third-party controller stays narrowband. Playback is unaffected.
- Sleep/wake and a boot after a full power cut are not verified yet.
- Only `0489:e10a` with ROM `0x00190200` is tested. The other WCN785x USB ids from Linux are in `Info.plist` and share the firmware; WCN6855 would need its own files.

Diagnostics: the kext publishes `Log`, `Stage` and `LastResult` in the I/O Registry (`ioreg -r -c BluetoothWCN7850 -w0`); `-qcabtoff` disables it, `qcabt_stage=<n>` parks it before a stage. `tools/qcabt` does the same download from userspace. The bring-up log with every wrong turn is in [docs/04-bringup.md](docs/04-bringup.md).

## Building

Command Line Tools, git, curl, python3 (Xcode for the two companion kexts, whose upstream builds are Xcode projects):

```bash
bash scripts/build-continuity.sh
```

This runs the host tests, fetches the firmware and the pinned dependencies, and builds all three kexts. The steps can also be run one at a time: `scripts/fetch-firmware.sh && ./build.sh` for the loader, `scripts/build-bluetoolfixup.sh` and `scripts/build-intelbtpatcher.sh` for the companions.

Results in `build/`. The firmware is fetched from linux-firmware (pinned release, checksums in `firmware/SHA256SUMS`) and embedded; it is Qualcomm's, redistributable in binary form, not committed.

## Credits

Linux `drivers/bluetooth/btusb.c` and linux-firmware; [IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) and [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM) for the pattern and the companion kexts; [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK). MIT, see `LICENSE`; the firmware has its own licence.
