#!/usr/bin/env python3
"""Read existing AWDL capture counters over SSH; save anonymous timing only.

Drains the diagnostic ring; does not change radio settings. Packet bodies,
addresses and service records stay in memory and are never written to disk.
Peer numbers are meaningful only within one run. This is a diagnostic command,
not a service or a runtime dependency of the driver.
"""
import argparse
from collections import Counter
import json
import os
import re
import struct
import subprocess
import time

HEADER = struct.Struct('<IIHBBIbBH')
BSSID = bytes.fromhex('002500ff9473')


class Timing:
    def __init__(self):
        self.peers = {}
        self.events = []
        self.counts = Counter()

    def ingest(self, raw, save=True):
        pos = 0
        while pos + HEADER.size <= len(raw):
            n, lo, flags, decap, ch, seq, rssi, rate, hi = HEADER.unpack_from(raw, pos)
            if n == 0xffffffff:
                break
            pos += HEADER.size
            if n > 4096 or pos + n > len(raw):
                self.counts['malformed_records'] += 1
                break
            frame = raw[pos:pos + n]
            pos += (n + 3) & ~3
            self.counts['records'] += 1
            if not save or len(frame) < 24 or frame[16:22] != BSSID:
                continue
            source = frame[10:16]
            if source[0] & 1 or source == bytes(6):
                continue
            peer = self.peers.setdefault(source, len(self.peers) + 1)
            event = dict(peer=peer, t=(hi << 32) | lo, ch=ch, rssi=rssi)
            if (frame[0] >> 2) & 3 == 2:
                self.events.append(dict(event, kind='data'))
                continue
            if flags & 32:
                frame = frame[:-4]
            if (len(frame) < 40 or frame[0] != 0xd0 or frame[1] & 0xc7 or
                    frame[22] & 15 or frame[24:30] != bytes.fromhex('7f0017f20810') or
                    frame[30] not in (0, 3)):
                continue
            p, sync, channels = 40, None, None
            valid = True
            while p < len(frame):
                if p + 3 > len(frame):
                    valid = False
                    break
                tag = frame[p]
                size = int.from_bytes(frame[p + 1:p + 3], 'little')
                p += 3
                if p + size > len(frame):
                    valid = False
                    break
                v = frame[p:p + size]
                p += size
                if tag == 4:
                    if sync is not None or size < 31:
                        valid = False
                        break
                    sync = dict(aw=int.from_bytes(v[29:31], 'little'),
                                down=int.from_bytes(v[1:3], 'little'),
                                period=int.from_bytes(v[5:7], 'little'), ext=v[27])
                elif tag == 18:
                    if (channels is not None or size < 38 or v[0] != 15 or
                            v[1] not in (1, 3) or v[2:6] != b'\x00\x03\xff\xff'):
                        valid = False
                        break
                    channels = [0 if v[6+2*i:8+2*i] == b'\xff\xff' else
                                v[7+2*i] if v[1] == 1 else v[6+2*i] for i in range(16)]
            if (valid and sync and channels and sync['ext'] == 4 and
                    8 <= sync['period'] <= 64 and sync['down'] <= sync['period'] * 4):
                self.events.append(dict(event, kind='sync', subtype=frame[30],
                                        channels=channels, **sync))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, help='Existing SSH target (user@host)')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 45:
        parser.error('--seconds must be between 1 and 45')
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    timing, samples = Timing(), []
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', args.host]

    def read():
        return subprocess.run(ssh + ['sysctl -b debug.qwz.moncap'],
                              capture_output=True, timeout=8, check=True).stdout

    def stats():
        result = subprocess.run(ssh + ['ioreg -l -w0 -r -k QwzAWDLRadioStats'],
                                capture_output=True, text=True, timeout=8, check=True)
        for line in result.stdout.splitlines():
            if '"QwzAWDLRadioStats" = ' in line:
                return {k: int(v) for k, v in re.findall(r'"([A-Za-z0-9]+)"=(\d+)', line)}
        return {}

    with os.fdopen(descriptor, 'w') as file:
        start = time.monotonic()
        interruption = None
        try:
            timing.ingest(read(), save=False)  # discard the old, possibly full ring
            start = time.monotonic()
            while time.monotonic() - start < args.seconds:
                timing.ingest(read())
                samples.append(dict(elapsed=time.monotonic() - start, stats=stats()))
                time.sleep(0.5)
        except (subprocess.SubprocessError, KeyboardInterrupt) as error:
            # A radio reset can close SSH mid-sample. Retain the observations
            # already collected instead of leaving a zero-byte diagnostic.
            interruption = type(error).__name__
            raise
        finally:
            json.dump(dict(duration=time.monotonic() - start, counts=dict(timing.counts),
                           events=timing.events, samples=samples, interruption=interruption), file, indent=2)
    print(json.dumps(dict(path=args.output, events=len(timing.events),
                          counts=dict(timing.counts))))


if __name__ == '__main__':
    main()
