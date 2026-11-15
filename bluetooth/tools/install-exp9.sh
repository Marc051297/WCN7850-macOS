#!/bin/bash
# Run locally on the target after SCP. One bundle, no config edits or reboot.
set -euo pipefail
PAYLOAD=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-install}
case "$MODE" in
    install) BUNDLE=BlueToolFixup-1.1.0-experimental.9.kext;;
    --rollback) BUNDLE=rollback/BlueToolFixup-1.1.0-experimental.8.kext;;
    *) echo 'Usage: bash install.sh [--rollback]' >&2; exit 1;;
esac
die() { echo "$*" >&2; exit 1; }
verify_file() {
    local file=$1 expected=$2
    [[ "$expected" =~ ^[0-9a-f]{64}$ ]] || die 'Invalid expected SHA-256.'
    test -f "$file" && test ! -L "$file" || die "Missing/linked file: $file"
    test "$(shasum -a 256 "$file" | awk '{print $1}')" = "$expected" || die "Checksum mismatch: $file"
}
verify_bundle() {
    local dir=$1 prefix=$2 hash relative count=0
    while read -r hash relative; do
        case "$relative" in "$prefix/"*)
            verify_file "$dir/${relative#"$prefix/"}" "$hash"
            count=$((count+1));;
        esac
    done < "$PAYLOAD/SHA256SUMS"
    test "$count" -ge 2 || die 'Incomplete bundle manifest.'
}
test "$(uname -m)" = x86_64 || die 'Run this installer on the target Intel/AMD Mac.'
test "$(sw_vers -buildVersion)" = 25G342 || die 'This package targets macOS build 25G342.'
if test "$MODE" = install; then
    BOOTARGS=$(sysctl -n kern.bootargs)
    case " $BOOTARGS " in *' -qcaadv '*) ;; *) die 'Missing existing -qcaadv boot flag.';; esac
fi
test -w "$PAYLOAD" || die 'Package directory must be writable for main-disk backups.'
(cd "$PAYLOAD" && shasum -a 256 -c SHA256SUMS >/dev/null) || die 'Package verification failed.'
verify_bundle "$PAYLOAD/$BUNDLE" "$BUNDLE"
PS=$(diskutil info / | awk -F: '/APFS Physical Store/{gsub(/ /,"",$2);print $2}')
[[ "$PS" =~ ^disk[0-9]+s[0-9]+$ ]] || die 'Could not identify one APFS physical store.'
EFI=${PS%s*}s1
sudo diskutil mount "$EFI"
MP=$(diskutil info "$EFI" | awk -F: '/Mount Point/{sub(/^ +/,"",$2);print $2}')
test -n "$MP" && test -f "$MP/EFI/OC/config.plist" || die 'Mounted EFI has no OpenCore config.'
plutil -lint "$MP/EFI/OC/config.plist"
KEXTS="$MP/EFI/OC/Kexts"
DST="$KEXTS/BlueToolFixup.kext"
test -d "$KEXTS" && test ! -L "$KEXTS" && test ! -L "$DST" || die 'Unexpected Kexts path.'
test -f "$DST/Contents/MacOS/BlueToolFixup" || die 'Existing BlueToolFixup is missing.'
FREE_KB=$(df -Pk "$MP" | awk 'NR==2 {print $4}')
NEEDED_KB=$(du -sk "$PAYLOAD/$BUNDLE" | awk '{print $1}')
[[ "$FREE_KB" =~ ^[0-9]+$ && "$NEEDED_KB" =~ ^[0-9]+$ ]] || die 'Could not check EFI space.'
test "$FREE_KB" -ge "$((NEEDED_KB+256))" || die 'Insufficient EFI space; nothing was replaced.'
STAMP=$(date +%Y%m%d%H%M%S)-$$
BACKUP="$PAYLOAD/backups/pre-$MODE-$STAMP"
STAGE="$KEXTS/.bt-exp9-stage-$STAMP"
CFG_HASH=$(shasum -a 256 "$MP/EFI/OC/config.plist" | awk '{print $1}')
mkdir -p "$BACKUP"
sudo cp -R "$DST" "$BACKUP/BlueToolFixup.kext"
sudo chown -R "$(id -u):$(id -g)" "$BACKUP"
# Check both executable and plist against the live original before any swap.
for part in Contents/MacOS/BlueToolFixup Contents/Info.plist; do
    verify_file "$BACKUP/BlueToolFixup.kext/$part" "$(shasum -a 256 "$DST/$part" | awk '{print $1}')"
done
(cd "$BACKUP" && find BlueToolFixup.kext -type f -exec shasum -a 256 '{}' \; > SHA256SUMS)
sudo mkdir "$STAGE"
rollback_failed_install() {
    local result=$? failed=0
    trap - EXIT INT TERM
    set +e
    if test -d "$STAGE/previous.kext"; then
        sudo rm -rf "$DST"
        sudo mv "$STAGE/previous.kext" "$DST" || failed=1
    fi
    if test "$failed" = 0; then
        sudo rm -rf "$STAGE"
        echo 'Install stopped; original kext retained/restored. Do not reboot into a partial update.' >&2
    else
        echo "Restore failed; do not reboot. Original remains in $STAGE/previous.kext; verified copy: $BACKUP" >&2
    fi
    test "$result" != 0 || result=1
    exit "$result"
}
trap rollback_failed_install EXIT
trap 'exit 1' INT TERM
sudo cp -R "$PAYLOAD/$BUNDLE" "$STAGE/BlueToolFixup.kext"
verify_bundle "$STAGE/BlueToolFixup.kext" "$BUNDLE"
sudo mv "$DST" "$STAGE/previous.kext"
sudo mv "$STAGE/BlueToolFixup.kext" "$DST"
verify_bundle "$DST" "$BUNDLE"
verify_file "$MP/EFI/OC/config.plist" "$CFG_HASH"
trap - EXIT INT TERM
sudo rm -rf "$STAGE/previous.kext"
sudo rmdir "$STAGE"
echo "Installed and SHA-256 verified: $BUNDLE"
echo "Previous bundle verified and saved on the main disk: $BACKUP"
echo 'Ready to reboot. No other kext or config setting was changed.'
