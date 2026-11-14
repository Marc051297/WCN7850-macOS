#!/bin/bash
# Run on the target Mac after SCP. Never edits config.plist or reboots.
set -euo pipefail
PAYLOAD=$(cd "$(dirname "$0")" && pwd)
WIFI=AirportWCN7850-build5-f2m-20261114.kext
BT=IntelBTPatcher-1.1.0-experimental.6.kext
die() { echo "$*" >&2; exit 1; }
verify() {
    local binary=$1 expected=$2
    [[ "$expected" =~ ^[0-9a-f]{32}$ ]] || die "Invalid expected checksum."
    test -f "$binary" || die "Missing executable: $binary"
    test "$(md5 -q "$binary")" = "$expected" || die "Checksum mismatch: $binary"
}
test "$(uname -m)" = x86_64 || die "Run this installer on the hackintosh."
test "$(sw_vers -buildVersion)" = 25G342 || die "This handoff targets macOS build 25G342."
BOOTARGS=$(sysctl -n kern.bootargs)
case " $BOOTARGS " in *" -qwzawdlnative "*) ;; *) die "Missing existing -qwzawdlnative boot flag.";; esac
case " $BOOTARGS " in *" -qcaadv "*) ;; *) die "Missing existing -qcaadv boot flag.";; esac
EXPECTED_WIFI=$(awk -v p="$WIFI/Contents/MacOS/AirportWCN7850" '$2==p {print $1}' "$PAYLOAD/MD5SUMS")
EXPECTED_BT=$(awk -v p="$BT/Contents/MacOS/IntelBTPatcher" '$2==p {print $1}' "$PAYLOAD/MD5SUMS")
verify "$PAYLOAD/$WIFI/Contents/MacOS/AirportWCN7850" "$EXPECTED_WIFI"
verify "$PAYLOAD/$BT/Contents/MacOS/IntelBTPatcher" "$EXPECTED_BT"
PS=$(diskutil info / | awk -F: '/APFS Physical Store/{gsub(/ /,"",$2);print $2}')
[[ "$PS" =~ ^disk[0-9]+s[0-9]+$ ]] || die "Could not identify one APFS physical store."
EFI=${PS%s*}s1
sudo diskutil mount "$EFI"
MP=$(diskutil info "$EFI" | awk -F: '/Mount Point/{sub(/^ +/,"",$2);print $2}')
test -n "$MP" && test -f "$MP/EFI/OC/config.plist" || die "Mounted EFI has no OpenCore config."
plutil -lint "$MP/EFI/OC/config.plist"
KEXTS="$MP/EFI/OC/Kexts"
for name in AirportWCN7850 IntelBTPatcher; do
    test -f "$KEXTS/$name.kext/Contents/MacOS/$name" || die "Existing $name.kext is missing."
    test ! -L "$KEXTS/$name.kext" || die "Unexpected symlink: $name.kext"
done
STAMP=$(date +%Y%m%d%H%M%S)-$$
BACKUP="$MP/EFI/OC/Kext-backups/pre-f2m-exp6-$STAMP"
STAGE="$KEXTS/.f2m-stage-$STAMP"
sudo mkdir -p "$BACKUP" "$STAGE"
rollback() {
    local result=$?
    trap - EXIT INT TERM
    set +e
    local failed=0
    for name in AirportWCN7850 IntelBTPatcher; do
        if test -d "$BACKUP/$name.kext"; then
            sudo rm -rf "$KEXTS/$name.kext"
            sudo mv "$BACKUP/$name.kext" "$KEXTS/$name.kext" || failed=1
        fi
    done
    sudo rm -rf "$STAGE"
    if test "$failed" = 0; then
        echo "Install stopped; original kexts restored. Do not reboot into a partial update." >&2
    else
        echo "Restore could not finish. Do not reboot. Backups: $BACKUP" >&2
    fi
    test "$result" != 0 || result=1
    exit "$result"
}
trap rollback EXIT
trap 'exit 1' INT TERM
sudo cp -R "$PAYLOAD/$WIFI" "$STAGE/AirportWCN7850.kext"
sudo cp -R "$PAYLOAD/$BT" "$STAGE/IntelBTPatcher.kext"
verify "$STAGE/AirportWCN7850.kext/Contents/MacOS/AirportWCN7850" "$EXPECTED_WIFI"
verify "$STAGE/IntelBTPatcher.kext/Contents/MacOS/IntelBTPatcher" "$EXPECTED_BT"
for name in AirportWCN7850 IntelBTPatcher; do
    sudo mv "$KEXTS/$name.kext" "$BACKUP/$name.kext"
    sudo mv "$STAGE/$name.kext" "$KEXTS/$name.kext"
done
verify "$KEXTS/AirportWCN7850.kext/Contents/MacOS/AirportWCN7850" "$EXPECTED_WIFI"
verify "$KEXTS/IntelBTPatcher.kext/Contents/MacOS/IntelBTPatcher" "$EXPECTED_BT"
sudo rmdir "$STAGE"
trap - EXIT INT TERM
echo "Installed f2m + IntelBTPatcher experimental.6. Checksums verified."
echo "Backup: $BACKUP"
echo "Ready to reboot."
