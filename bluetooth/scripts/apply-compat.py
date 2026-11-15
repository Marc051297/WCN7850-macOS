#!/usr/bin/env python3
"""Apply the checked-in compatibility patches to pinned dependencies, idempotently."""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def apply(tree, name):
    patch = ROOT / 'patches' / name
    options = dict(cwd=tree, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], **options).returncode == 0:
        return
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=tree, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=tree, check=True)

def main():
    name = sys.argv[1]
    tree = ROOT / 'deps' / name
    if name == 'BrcmPatchRAM':
        destination = tree / 'BrcmPatchRAM'
        apply(tree, 'bluetoolfixup-continuity.patch')
        files = ['QcaContinuityPatches.hpp', 'QcaBluetoothHci.hpp',
                 'QcaValidatedPages.hpp', 'QcaVerifyLayout.hpp',
                 'QcaPeerAddress25G342.hpp', 'QcaPageGuard.inc']
    elif name == 'IntelBluetoothFirmware':
        destination = tree / 'IntelBTPatcher'
        apply(tree, 'intelbtpatcher-continuity.patch')
        files = ['QcaBluetoothHci.hpp', 'QcaBluetoothHooks.inc', 'QcaBluetoothAsync.inc']
    else:
        raise SystemExit(f'Unsupported dependency: {name}')
    for file in files:
        shutil.copy2(ROOT / 'compat' / file, destination / file)

if __name__ == '__main__':
    main()
