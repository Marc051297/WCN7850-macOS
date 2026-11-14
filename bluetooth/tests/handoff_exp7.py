#!/usr/bin/env python3
"""Exercise the real one-bundle installer against a disposable EFI."""
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def command(path,text):
    path.write_text('#!/bin/bash\nset -e\n'+text);path.chmod(0o755)

for mode in ['success','bad-source','bad-copy','fail-swap']:
    with tempfile.TemporaryDirectory(prefix='qca-install-test-') as tmp:
        base=Path(tmp);payload=base/'payload';cmds=base/'commands';oc=base/'EFI/EFI/OC'
        payload.mkdir();cmds.mkdir();oc.mkdir(parents=True)
        config=plistlib.dumps({'Kernel':{'Add':[]}});(oc/'config.plist').write_bytes(config)
        names=['BlueToolFixup','IntelBTPatcher','BluetoothWCN7850','AirportWCN7850']
        for name in names:
            path=oc/f'Kexts/{name}.kext/Contents/MacOS/{name}'
            path.parent.mkdir(parents=True);path.write_bytes(('old '+name).encode())
        relative='BlueToolFixup-1.1.0-experimental.7.kext/Contents/MacOS/BlueToolFixup'
        binary=payload/relative;binary.parent.mkdir(parents=True);binary.write_bytes(b'new BlueToolFixup')
        (payload/'MD5SUMS').write_text(hashlib.md5(binary.read_bytes()).hexdigest()+'  '+relative+'\n')
        if mode=='bad-source':binary.write_bytes(b'corrupt')
        shutil.copy2(ROOT/'tools/install-exp7.sh',payload/'install.sh')
        command(cmds/'uname','echo x86_64\n')
        command(cmds/'sw_vers','echo 25G342\n')
        command(cmds/'sysctl','echo "-qcaadv"\n')
        command(cmds/'diskutil',r'''
if test "$1" = mount; then touch "$MOCK_ROOT/mounted";exit 0;fi
if test "$2" = /;then echo 'APFS Physical Store: disk0s2';else echo "Mount Point: $MOCK_ROOT/EFI";fi
''')
        command(cmds/'sudo',r'''
if test "$MOCK_MODE" = fail-swap && test "$1" = mv && [[ "$2" = *".bt-exp7-stage-"*"/BlueToolFixup.kext" ]];then
 echo 'Injected swap failure' >&2;exit 7
fi
if test "$MOCK_MODE" = bad-copy && test "$1" = cp && [[ "$4" = *".bt-exp7-stage-"*"/BlueToolFixup.kext" ]];then
 "$@";echo corrupt > "$4/Contents/MacOS/BlueToolFixup";exit 0
fi
exec "$@"
''')
        env=dict(os.environ,PATH=str(cmds)+':'+os.environ['PATH'],MOCK_ROOT=str(base),MOCK_MODE=mode)
        r=subprocess.run(['/bin/bash',str(payload/'install.sh')],env=env,capture_output=True,text=True)
        assert (r.returncode==0)==(mode=='success'),(mode,r.stdout,r.stderr)
        assert (oc/'config.plist').read_bytes()==config
        for name in names:
            value=('new' if mode=='success' and name=='BlueToolFixup' else 'old')+' '+name
            assert (oc/f'Kexts/{name}.kext/Contents/MacOS/{name}').read_bytes()==value.encode()
        if mode=='success':
            saved=list((oc/'Kext-backups').glob('*/BlueToolFixup.kext/Contents/MacOS/BlueToolFixup'))
            assert len(saved)==1 and saved[0].read_bytes()==b'old BlueToolFixup'
        if mode=='bad-source':assert not (base/'mounted').exists()
        if mode=='bad-copy':assert 'Checksum mismatch' in r.stderr
        if mode=='fail-swap':assert 'Injected swap failure' in r.stderr
        assert not list((oc/'Kexts').glob('.bt-exp7-stage-*'))
print('PASS: exp7 installer verifies source/stage, preserves three other kexts/config, backs up and rolls back failed copies/swaps')
