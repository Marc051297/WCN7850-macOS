# Install

For a hackintosh with OpenCore and a Qualcomm WCN7850. Tested on an MSI MAG X870E Tomahawk WiFi with macOS Tahoe 26.7.2 (25G332) and 26.7.3 (25G342), SMBIOS MacPro7,1.

Keep a bootable copy of your working EFI on a USB stick before you start.

## 1. Map the USB ports first

macOS uses only the first 15 ports of each USB controller. On the tested board the Bluetooth controller sits on port 17 of the chipset controller (`XH00`), so without a port map it never appears.

Make a map with [USBToolBox](https://github.com/USBToolBox/tool) that includes the Bluetooth port. Before installing anything from here, check that the device is listed:

```
ioreg -p IOUSB | grep -i -B2 -A2 e10a
```

## 2. Kexts

Take the four kexts from the [latest release](https://github.com/Marc051297/WCN7850-macOS/releases), or build them (see the README), and copy them to `EFI/OC/Kexts`. In `Kernel → Add`, in this order after `Lilu.kext`:

1. `BlueToolFixup.kext`
2. `IntelBTPatcher.kext`
3. `BluetoothWCN7850.kext`
4. `AirportWCN7850.kext`, with `MinKernel` `25.0.0`

All three Bluetooth kexts must come from this repository. The stock BlueToolFixup and IntelBTPatcher lack the changes that make LE devices pair and that let iPhones find the Mac.

Do not also load itlwm, AirportItlwm, BTLEAdvFixup or IntelBTUnified. If your config hides the Wi-Fi card with an SSDT such as `SSDT-Disable_Network_GPP7`, disable that SSDT.

## 3. Boot arguments

Add to `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`:

```
itlwm_cc=US -qwzawdlnative -itlawdlnativeup -itlawdlcoexdata -itlawdlmitx -itlawdlelicit -btlfxboardid -btlfxlemask -qcaadv
```

Use your own country code in `itlwm_cc`. What each argument does is in [boot-arguments.md](boot-arguments.md).

For a first boot, leave out `-qcaadv` and the five AirDrop arguments. Check that plain Wi-Fi and Bluetooth work, then add them.

## 4. Bluetooth NVRAM values

Under the same GUID in `NVRAM → Add`:

| Key | Type | Value |
|---|---|---|
| `bluetoothExternalDongleFailed` | Data | `00` |
| `bluetoothInternalControllerInfo` | Data | 14 zero bytes |

Put the same two keys under `NVRAM → Delete` so OpenCore rewrites them at each boot.

## 5. Wi-Fi device property

In `DeviceProperties → Add`, for the PCI path of the Wi-Fi card:

| Key | Type | Value |
|---|---|---|
| `pci-aspm-default` | Number | `0` |

Without it macOS enables ASPM L1 on the link, and the link drops during idle and display sleep.

## 6. Firmware settings that mattered on the tested board

- ErP enabled. It cuts standby power to the module in S3; the driver reloads the firmware on every wake.
- Resume by PCI-E device disabled.

## 7. Check after the reboot

```
ioreg -l -w0 | grep -o '"QwzAWDLRadioBuild" = "[^"]*"'
ifconfig awdl0 | head -3
ioreg -l -w0 -d 1 | grep QcaContinuity
```

- The first line names the Wi-Fi build.
- `awdl0` should exist and be up once Wi-Fi has joined a network.
- With `-qcaadv`, `QcaContinuityBuildAllowed` should be `Yes`.

Then test in this order: join your network, pair a Bluetooth device, open the AirDrop window in Finder, send a file each way.

## Logs

The Wi-Fi driver keeps a log ring. To read it:

```
sudo sysctl debug.qwz.publish=1
ioreg -l -w0 -r -k QwzMacLog | LC_ALL=C sed -n '/"QwzMacLog" = "/,/^"$/p' > ~/qwz-ring.txt
```

The ring holds addresses of nearby networks and devices. Remove them before posting a log in an issue.

## Updating macOS

1. Boot once without `-qcaadv`, then update.
2. After the update, AirDrop and the Bluetooth advertising path stay off until the new build has been verified in the source. Plain Wi-Fi and Bluetooth keep working.

## Turning things off

| To turn off | Do this |
|---|---|
| The Wi-Fi driver | `-qwzoff` |
| AirDrop | Remove `-qwzawdlnative` |
| The Bluetooth advertising path | Remove `-qcaadv`, or add `-qcaadvoff` |
| The Bluetooth firmware loader | `-qcabtoff` |
