#!/usr/bin/env python3
"""Fetch exact build dependencies without resetting an existing checkout."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'dependencies.lock.json').read_text())

def prepare(name):
    spec = LOCK[name]
    destination = ROOT / 'deps' / name
    if not destination.exists():
        destination.parent.mkdir(exist_ok=True)
        subprocess.run(['git', 'init', str(destination)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['git', '-C', str(destination), 'remote', 'add', 'origin', spec['url']], check=True)
        subprocess.run(['git', '-C', str(destination), 'fetch', '--depth', '1', 'origin', spec['commit']], check=True)
        subprocess.run(['git', '-C', str(destination), 'checkout', '--detach', spec['commit']], check=True)
    actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != spec['commit']:
        raise SystemExit(f'{destination}: expected {spec["commit"]}, got {actual}; use a separate deps checkout')

if __name__ == '__main__':
    for name in sys.argv[1:]:
        prepare(name)
