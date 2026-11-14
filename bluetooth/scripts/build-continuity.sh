#!/bin/bash
# Build only; never mounts/writes an EFI or changes running services.
set -euo pipefail
cd "$(dirname "$0")/.."
bash scripts/test-continuity.sh
bash scripts/fetch-firmware.sh
bash build.sh
bash scripts/build-bluetoolfixup.sh
bash scripts/build-intelbtpatcher.sh
python3 -I scripts/package-continuity.py
