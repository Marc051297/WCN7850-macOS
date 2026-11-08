#!/bin/bash
# fetch-firmware-c7.sh -> firmware-c7/FwBinary.cpp and firmware-c7/qwz_fw.h
# K5 OBSD-6: the WCN7850 firmware of linux-firmware tag 20261019 (WLAN.HMT.1.1.c7-00108), for the separate c7 kext.
# Same steps as scripts/fetch-firmware.sh (TAG=20260309, c5-00302), which stays untouched: its tag and its
# licence path would fail here (the licence moved to LICENSES/ at this tag). Nothing under firmware/ is written.
# The firmware is Qualcomm's (LICENSE.QualcommAtheros_ath10k): redistributable in binary form, so it is fetched
# at build time and never committed (firmware-c7/dl and firmware-c7/FwBinary.cpp are generated).
# The script lives in firmware-c7/ and works unchanged if it is moved to scripts/ (it runs from the tree root).
set -euo pipefail
cd "$(dirname "$0")/.."
TAG=20261019
BASE=https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$TAG
DL=firmware-c7/dl
mkdir -p "$DL"

fetch() {  # fetch <path in linux-firmware> <local name>
  [ -s "$DL/$2" ] || curl -fsSL --retry 3 -o "$DL/$2" "$BASE/$1"
}
fetch ath12k/WCN7850/hw2.0/amss.bin    qwz-wcn7850-hw2.0-amss
fetch ath12k/WCN7850/hw2.0/m3.bin      qwz-wcn7850-hw2.0-m3
fetch ath12k/WCN7850/hw2.0/board-2.bin qwz-wcn7850-hw2.0-board-2
fetch LICENSES/LICENSE.QualcommAtheros_ath10k LICENSE.QualcommAtheros_ath10k

(cd "$DL" && shasum -a 256 -c ../SHA256SUMS)

python3 -I scripts/fw_gen.py firmware-c7/FwBinary.cpp "$DL"/qwz-*
cp firmware/qwz_fw.h firmware-c7/qwz_fw.h
