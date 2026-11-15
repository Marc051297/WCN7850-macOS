#!/usr/bin/env python3
"""Run the real install/rollback script against a disposable EFI and faults."""
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def command(path, text):
    path.write_text('#!/bin/bash\nset -e\n'+text)
    path.chmod(0o755)


for mode in ('success', 'bad-source', 'bad-plist', 'bad-copy',
             'fail-old-move', 'fail-swap', 'full-efi', 'rollback'):
    with tempfile.TemporaryDirectory(prefix='qca-exp9-install-') as tmp:
        base = Path(tmp)
        payload, cmds, oc = base/'payload', base/'commands', base/'EFI/EFI/OC'
        payload.mkdir(); cmds.mkdir(); oc.mkdir(parents=True)
        config = plistlib.dumps({'Kernel': {'Add': []}})
        (oc/'config.plist').write_bytes(config)
        names = ('BlueToolFixup', 'IntelBTPatcher', 'BluetoothWCN7850', 'AirportWCN7850')
        for name in names:
            bundle = oc/f'Kexts/{name}.kext/Contents'
            (bundle/'MacOS').mkdir(parents=True)
            (bundle/f'MacOS/{name}').write_bytes(('old '+name).encode())
            (bundle/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': name}))
        bundles = ['BlueToolFixup-1.1.0-experimental.9.kext',
                   'rollback/BlueToolFixup-1.1.0-experimental.8.kext']
        for i, bundle in enumerate(bundles):
            path = payload/bundle/'Contents'
            (path/'MacOS').mkdir(parents=True)
            (path/'MacOS/BlueToolFixup').write_bytes(b'new BlueToolFixup' if i == 0 else b'sealed exp8')
            (path/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'BlueToolFixup'}))
        shutil.copy2(ROOT/'tools/install-exp9.sh', payload/'install.sh')
        manifest = []
        for file in sorted(payload.rglob('*')):
            if file.is_file():
                manifest.append(hashlib.sha256(file.read_bytes()).hexdigest()+'  '+str(file.relative_to(payload)))
        (payload/'SHA256SUMS').write_text('\n'.join(manifest)+'\n')
        if mode in ('bad-source', 'bad-plist'):
            part = 'MacOS/BlueToolFixup' if mode == 'bad-source' else 'Info.plist'
            (payload/bundles[0]/'Contents'/part).write_bytes(b'corrupt')
        command(cmds/'uname', 'echo x86_64\n')
        command(cmds/'sw_vers', 'echo 25G342\n')
        command(cmds/'sysctl', 'echo "-qcaadv"\n')
        command(cmds/'diskutil', r'''
if test "$1" = mount; then touch "$MOCK_ROOT/mounted";exit 0;fi
if test "$2" = /;then echo 'APFS Physical Store: disk0s2';else echo "Mount Point: $MOCK_ROOT/EFI";fi
''')
        command(cmds/'df', r'''
echo 'Filesystem 1024-blocks Used Available Capacity Mounted'
if test "$MOCK_MODE" = full-efi;then echo 'disk 100 99 1 99% /efi';else echo 'disk 1000000 1 999999 1% /efi';fi
''')
        command(cmds/'sudo', r'''
if test "$MOCK_MODE" = fail-old-move && test "$1" = mv && [[ "$2" = *"Kexts/BlueToolFixup.kext" ]];then
 echo 'Injected old move failure' >&2;exit 7
fi
if test "$MOCK_MODE" = fail-swap && test "$1" = mv && [[ "$2" = *".bt-exp9-stage-"*"/BlueToolFixup.kext" ]];then
 echo 'Injected swap failure' >&2;exit 7
fi
if test "$MOCK_MODE" = bad-copy && test "$1" = cp && [[ "$4" = *".bt-exp9-stage-"*"/BlueToolFixup.kext" ]];then
 "$@";echo corrupt > "$4/Contents/MacOS/BlueToolFixup";exit 0
fi
exec "$@"
''')
        env = dict(os.environ, PATH=str(cmds)+':'+os.environ['PATH'], MOCK_ROOT=str(base), MOCK_MODE=mode)
        args = ['/bin/bash', str(payload/'install.sh')]+(['--rollback'] if mode == 'rollback' else [])
        result = subprocess.run(args, env=env, capture_output=True, text=True)
        assert (result.returncode == 0) == (mode in ('success', 'rollback')), (mode, result.stdout, result.stderr)
        assert (oc/'config.plist').read_bytes() == config
        for name in names:
            expected = ('old '+name).encode()
            if name == 'BlueToolFixup' and mode == 'success': expected = b'new BlueToolFixup'
            if name == 'BlueToolFixup' and mode == 'rollback': expected = b'sealed exp8'
            assert (oc/f'Kexts/{name}.kext/Contents/MacOS/{name}').read_bytes() == expected
        assert not list((oc/'Kexts').glob('.bt-exp9-stage-*'))
        if mode in ('bad-source', 'bad-plist'):
            assert not (base/'mounted').exists()
        if mode == 'full-efi': assert not (payload/'backups').exists()
        if mode in ('success', 'rollback', 'bad-copy', 'fail-old-move', 'fail-swap'):
            copies = list((payload/'backups').glob('*/BlueToolFixup.kext/Contents/MacOS/BlueToolFixup'))
            assert len(copies) == 1 and copies[0].read_bytes() == b'old BlueToolFixup'
print('PASS: exp9 install/rollback; source/plist/copy corruption, full EFI, failed swaps, main-disk backup, other kexts/config preserved')
