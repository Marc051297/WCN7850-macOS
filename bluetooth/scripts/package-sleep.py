#!/usr/bin/env python3
"""Make an immutable local experimental.9 test package from committed sources.

The user tests and installs the single changed bundle. A sealed experimental.8
bundle is included for rollback; this command never installs or publishes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
RELEASE = 'WCN7850-bt-exp9-20261115'
ROLLBACK_SHA = '67c2967a312d96e464012cace9ac182e82c699253e0adf14454a9cd44166698a'


def output(*args):
    return subprocess.check_output(args, cwd=REPO, text=True).strip()


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rollback-bundle', type=Path, required=True)
    parser.add_argument('--source-base', required=True,
                        help='Commit before the experimental.9 changes, for the review patch')
    parser.add_argument('--output', type=Path, default=ROOT/'build'/RELEASE)
    args = parser.parse_args()
    args.rollback_bundle = args.rollback_bundle.resolve()
    target = args.output.resolve()
    if target.exists() or target.with_suffix('.zip').exists():
        raise SystemExit('Refusing to overwrite an existing versioned package')
    if output('git', 'status', '--porcelain'):
        raise SystemExit('Commit the reviewed source changes before packaging')
    new = ROOT/'build/BlueToolFixup.kext'
    new_exe = new/'Contents/MacOS/BlueToolFixup'
    old_exe = args.rollback_bundle/'Contents/MacOS/BlueToolFixup'
    if digest(old_exe) != ROLLBACK_SHA:
        raise SystemExit('Rollback is not the sealed, tested experimental.8 executable')
    if b'experimental.9-sleep-guards' not in new_exe.read_bytes() or b'/Users/' in new_exe.read_bytes():
        raise SystemExit('Missing revision or private build paths in the new executable')
    if output('xcrun', 'lipo', '-archs', str(new_exe)) != 'x86_64':
        raise SystemExit('Expected x86_64 bundle')
    if output('xcrun', 'nm', '-uj', str(new_exe)) != output('xcrun', 'nm', '-uj', str(old_exe)):
        raise SystemExit('New imports differ from the tested baseline')
    info = plistlib.loads((new/'Contents/Info.plist').read_bytes())
    if info['CFBundleExecutable'] != 'BlueToolFixup':
        raise SystemExit('Unexpected bundle executable')
    commit = output('git', 'rev-parse', 'HEAD')
    source_base = output('git', 'rev-parse', args.source_base+'^{commit}')
    subprocess.run(['git', 'merge-base', '--is-ancestor', source_base, commit], cwd=REPO, check=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='qca-exp9-package-', dir=target.parent) as tmp:
        stage = Path(tmp)/RELEASE
        stage.mkdir()
        shutil.copytree(new, stage/'BlueToolFixup-1.1.0-experimental.9.kext')
        shutil.copytree(args.rollback_bundle, stage/'rollback/BlueToolFixup-1.1.0-experimental.8.kext')
        shutil.copy2(ROOT/'tools/install-exp9.sh', stage/'install.sh')
        shutil.copy2(ROOT/'docs/experimental-9-sleep.md', stage/'CHANGES.md')
        (stage/'README.md').write_text('''# Bluetooth experimental.9 test package

Only BlueToolFixup is replaced. Keep the existing IntelBTPatcher, firmware loader
and Wi-Fi kext. Existing boot arguments remain; no new enabling argument is needed.

On the target, from this directory:

```sh
shasum -a 256 -c SHA256SUMS
bash install.sh && sudo reboot
```

After reboot, verify existing paired devices. With the LE mouse connected and
still, run `pmset sleepnow`, wait 30 seconds, then use the power button to wake.
Check `pmset -g log` for normal sleep and `Wake from Normal Sleep`. Then test
idle sleep (`sudo pmset -a sleep 1`) and the post-wake checklist in CHANGES.md.

To disable only the new policies, boot with `-qcabsleepoff`. Individual controls
are `-qcabhidoff` and `-qcabidleoff`. These are boot arguments, not shell commands.

To reinstall the sealed experimental.8 bundle, from this same directory:

```sh
bash install.sh --rollback && sudo reboot
```

The installer verifies the whole package and staged/installed bundle, checks EFI
space, and saves the previous bundle under this package's `backups/` directory on
the main disk before replacing anything. It neither edits config.plist nor
reboots. Both install and rollback replace BlueToolFixup only.

Hardware results for experimental.9 are pending. This local package is not a
published release. Private captures and Apple executables are excluded.
''')
        # Archive committed Bluetooth sources, excluding Wi-Fi implementation.
        archive = subprocess.check_output(['git', 'archive', '--format=tar', commit,
                                          'bluetooth', 'LICENSES.md'], cwd=REPO)
        (stage/'source.tar').write_bytes(archive)
        with tarfile.open(stage/'source.tar', 'r') as tar:
            with tarfile.open(stage/'source.tar.gz', 'w:gz') as gz:
                for item in tar:
                    gz.addfile(item, tar.extractfile(item) if item.isfile() else None)
        (stage/'source.tar').unlink()
        patch = subprocess.check_output(['git', 'diff', source_base, commit,
                                         '--', 'bluetooth', 'docs/boot-arguments.md',
                                         'README.md', 'CHANGELOG.md'], cwd=REPO)
        (stage/'experimental-9.patch').write_bytes(patch)
        manifest = dict(release=RELEASE, source_commit=commit, source_base=source_base, hardware_verified=False,
                        changed_bundles=['BlueToolFixup'], target_builds=['25G342'],
                        dependencies=json.loads((ROOT/'dependencies.lock.json').read_text()),
                        rollback_executable_sha256=ROLLBACK_SHA,
                        new_executable_sha256=digest(new_exe),
                        bundle_version=info['CFBundleVersion'])
        (stage/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        (stage/'MD5SUMS').write_text(''.join(hashlib.md5((stage/p/'Contents/MacOS/BlueToolFixup').read_bytes()).hexdigest()+
                                           '  '+str(p/'Contents/MacOS/BlueToolFixup')+'\n'
                                           for p in [Path('BlueToolFixup-1.1.0-experimental.9.kext'),
                                                     Path('rollback/BlueToolFixup-1.1.0-experimental.8.kext')]))
        paths = sorted(p for p in stage.rglob('*') if p.is_file())
        (stage/'SHA256SUMS').write_text(''.join(digest(p)+'  '+str(p.relative_to(stage))+'\n' for p in paths))
        stage.rename(target)
    with zipfile.ZipFile(target.with_suffix('.zip'), 'x', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(target.rglob('*')):
            if path.is_file(): archive.write(path, path.relative_to(target.parent))
    zipfile_path = target.with_suffix('.zip')
    zipfile_path.with_suffix('.zip.sha256').write_text(digest(zipfile_path)+'  '+zipfile_path.name+'\n')
    print(f'Package {target.name}; source {commit}; new SHA-256 {digest(new_exe)}')


if __name__ == '__main__':
    main()
