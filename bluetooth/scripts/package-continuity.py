#!/usr/bin/env python3
"""Validate and package the experimental stack, with sources and pinned upstreams."""
import gzip
import hashlib
import json
import plistlib
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
RELEASE = 'BluetoothWCN7850-1.1.0-experimental.6'
NAMES = ('BluetoothWCN7850', 'BlueToolFixup', 'IntelBTPatcher')

def output(*args, cwd=ROOT):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()

def main():
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    manifest = dict(release=RELEASE, hardware_verified=False, target_builds=['25G332', '25G342'],
                    early_boot_kernel='xnu-12377.161.15.701.48~2/RELEASE_X86_64',
                    usb_id='0489:e10a', base_commit=output('git', 'rev-parse', 'HEAD'),
                    dependencies=lock, files={})
    with tempfile.TemporaryDirectory(prefix='wcn7850-package-') as temporary:
        stage = Path(temporary) / RELEASE
        stage.mkdir()
        for name in NAMES:
            bundle = BUILD / (name + '.kext')
            info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
            assert info['CFBundleExecutable'] == name
            exe = bundle / 'Contents/MacOS' / name
            assert output('xcrun', 'lipo', '-archs', str(exe)) == 'x86_64', name
            symbols = output('xcrun', 'nm', str(exe))
            undefined = output('xcrun', 'nm', '-u', str(exe))
            for symbol in ('_kmod_info', '__realmain', '__antimain'):
                assert any(line.split()[-1:] == [symbol] for line in symbols.splitlines()), (name, symbol)
            assert 'kern_start' not in undefined and 'kern_stop' not in undefined, name
            if name != 'BluetoothWCN7850':
                assert 'as.vit9696.Lilu' in info['OSBundleLibraries']
                assert b'QcaContinuity' in exe.read_bytes(), name
            shutil.copytree(bundle, stage / bundle.name)
            manifest.setdefault('bundle_versions', {})[name] = info['CFBundleVersion']
        source = stage / 'source'
        source.mkdir()
        # Include newly added source files as well as the base repository's files.
        paths = output('git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z').split('\0')
        for name in paths:
            path = Path(name)
            if not name or path.parts[0] == 'release' or not (ROOT / path).is_file():
                continue
            destination = source / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, destination)
        snapshots = source / 'upstream-sources'
        snapshots.mkdir()
        for name, spec in lock.items():
            archive = subprocess.check_output(['git', 'archive', '--format=tar', spec['commit']], cwd=ROOT / 'deps' / name)
            (snapshots / (name + '.tar.gz')).write_bytes(gzip.compress(archive, mtime=0))
        # Include the exact modified dependency units as a review convenience too.
        for name, path in [('BrcmPatchRAM', 'BrcmPatchRAM/BlueToolFixup.cpp'),
                           ('IntelBluetoothFirmware', 'IntelBTPatcher/IntelBTPatcher.cpp')]:
            destination = source / 'modified-dependencies' / name / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / 'deps' / name / path, destination)
        licenses = stage / 'licenses'
        licenses.mkdir()
        for name in ('NOTICE.txt', 'LICENSE.QualcommAtheros_ath10k'):
            shutil.copy2(ROOT / 'firmware/dl' / name, licenses / name)
        shutil.copy2(ROOT / 'compat/LICENSE.GPL-3.0', licenses / 'GPL-3.0')
        shutil.copy2(ROOT / 'deps/BrcmPatchRAM/LICENSE', licenses / 'BrcmPatchRAM-LICENSE')
        shutil.copy2(ROOT / 'deps/IntelBluetoothFirmware/LICENSE', licenses / 'IntelBluetoothFirmware-LICENSE')
        shutil.copy2(ROOT / 'docs/continuity-experimental.md', stage / 'INSTALL.md')
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                manifest['files'][str(path.relative_to(stage))] = hashlib.sha256(path.read_bytes()).hexdigest()
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        archive_path = BUILD / (RELEASE + '.zip')
        with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(stage.rglob('*')):
                if path.is_file(): archive.write(path, path.relative_to(stage.parent))
        digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
        (BUILD / (RELEASE + '.zip.sha256')).write_text(digest + '  ' + archive_path.name + '\n')
        print(f'Validated 3 x86_64 bundles. Package: {archive_path}\nSHA-256: {digest}')

if __name__ == '__main__':
    main()
