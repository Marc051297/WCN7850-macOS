#!/usr/bin/env python3
"""Read AirDrop/hotspot logs; save filtered messages without raw pairing payloads."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess

spec = importlib.util.spec_from_file_location("continuity_report", Path(__file__).with_name("continuity_report.py"))
continuity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(continuity)

PREDICATE = '''
process == "sharingd" OR
(process == "rapportd" AND
 (eventMessage CONTAINS[c] "BLE" OR eventMessage CONTAINS[c] "hotspot")) OR
(process == "bluetoothd" AND
 (eventMessage CONTAINS[c] "Outgoing LE Connection complete" OR
  eventMessage CONTAINS[c] "Received connection complete event after successful cancellation" OR
  eventMessage CONTAINS[c] "LE Connection Cancelled with status" OR
  eventMessage CONTAINS[c] "Device connecting" OR
  eventMessage CONTAINS[c] "Bluetooth error - restarting" OR
  eventMessage CONTAINS[c] "Started advertising successfully")) OR
(process == "wifid" AND
 (eventMessage CONTAINS[c] "tether" OR
  eventMessage CONTAINS[c] "join failed"))
'''.replace("\n", " ")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="user@hackintosh.local")
    parser.add_argument("--local", action="store_true")
    parser.add_argument("--last", default="15m")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    command = ["/usr/bin/log", "show", "--style", "json", "--info", "--last", args.last,
               "--predicate", PREDICATE]
    if not args.local:
        command = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.host,
                   shlex.join(command) + " 2>/dev/null"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=55, check=True)
    rows = json.loads(result.stdout)
    events, counts = [], Counter()
    for row in rows:
        if not isinstance(row, dict):
            continue
        message = continuity.filtered_message(row.get("eventMessage", ""))
        if message is None:
            continue
        process = row.get("process", "") or Path(row.get("processImagePath", "")).name
        category = row.get("category", "")
        counts[(process, category)] += 1
        events.append({"time": row.get("timestamp"), "process": process,
                       "category": category, "message": message})
    report = {"captured_utc": datetime.now(timezone.utc).isoformat(),
              "host": "MacBook" if args.local else "hackintosh", "last": args.last,
              "counts": {f"{p}/{c}": n for (p, c), n in counts.most_common()}, "events": events}
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w") as file:
        json.dump(report, file, indent=2)
        file.write("\n")
    print(args.output)
    print(json.dumps(report["counts"], indent=2))


if __name__ == "__main__":
    main()
