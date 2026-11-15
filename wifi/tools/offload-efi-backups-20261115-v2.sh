#!/bin/bash
# Preserve and verify four historical test backups before freeing EFI space.
set -euo pipefail
umask 077
die() { echo "$*" >&2; exit 1; }
test "$(sw_vers -buildVersion)" = 25G342 || die "Run this on the 25G342 hackintosh."
PS=$(diskutil info / | awk -F: '/APFS Physical Store/{gsub(/ /,"",$2);print $2}')
[[ "$PS" =~ ^disk[0-9]+s[0-9]+$ ]] || die "Cannot identify one APFS store."
EFI=${PS%s*}s1
sudo diskutil mount "$EFI"
MP=$(diskutil info "$EFI" | awk -F: '/Mount Point/{sub(/^ +/,"",$2);print $2}')
test -n "$MP" && test -f "$MP/EFI/OC/config.plist" || die "Mounted EFI lacks OpenCore config."
BASE="$MP/EFI/OC/Kext-backups"
test -d "$BASE" && test ! -L "$BASE" || die "Backup directory missing or a symlink."
ACTIVE="$MP/EFI/OC/Kexts/AirportWCN7850.kext/Contents/MacOS/AirportWCN7850"
CURRENT=$(shasum -a 256 "$ACTIVE" | awk '{print $1}')
CONFIG=$(shasum -a 256 "$MP/EFI/OC/config.plist" | awk '{print $1}')
ARCHIVE="${WCN_ARCHIVE_ROOT:-$HOME}/EFI-Kext-backups-offload-$(date +%Y%m%d%H%M%S)-$$"
mkdir "$ARCHIVE"
NAMES=(
    pre-f2m-exp6-20261114154744-2285
    pre-f2n-20261114162057-1610
    pre-f2o-20261115061045-1037
    pre-f2p-20261115072543-2039
)
# Verify all copies before removing any source folder. Reject links rather
# than following them beyond the selected historical backup directory.
for name in "${NAMES[@]}"; do
    SRC="$BASE/$name"
    test -d "$SRC" && test ! -L "$SRC" || die "Historical backup missing or linked: $name"
    test -z "$(find "$SRC" -type l -print -quit)" || die "Linked entry in $name; nothing removed."
    (cd "$SRC" && find . -type f -exec shasum -a 256 {} + | LC_ALL=C sort) > "$ARCHIVE/$name.sha256"
    test -s "$ARCHIVE/$name.sha256" || die "Empty backup manifest: $name"
    sudo cp -R "$SRC" "$ARCHIVE/$name"
    sudo chown -R "$(id -u):$(id -g)" "$ARCHIVE/$name"
    (cd "$ARCHIVE/$name" && find . -type f -exec shasum -a 256 {} + | LC_ALL=C sort) > "$ARCHIVE/$name.copy.sha256"
    cmp "$ARCHIVE/$name.sha256" "$ARCHIVE/$name.copy.sha256" || die "Copy mismatch; no EFI backups removed."
done
for name in "${NAMES[@]}"; do
    SRC="$BASE/$name"
    (cd "$SRC" && find . -type f -exec shasum -a 256 {} + | LC_ALL=C sort) > "$ARCHIVE/$name.recheck.sha256"
    cmp "$ARCHIVE/$name.sha256" "$ARCHIVE/$name.recheck.sha256" || die "Source changed; this EFI backup was not removed."
    sudo rm -rf "$SRC"
done
test "$(shasum -a 256 "$ACTIVE" | awk '{print $1}')" = "$CURRENT" || die "Installed executable changed during offload."
test "$(shasum -a 256 "$MP/EFI/OC/config.plist" | awk '{print $1}')" = "$CONFIG" || die "Config changed during offload."
echo "Verified historical backups preserved at: $ARCHIVE"
echo "Installed kext and config checksums are unchanged; newest pre-f2q backup retained on EFI."
df -h "$MP"
