#!/bin/bash
n=${1:-60}
pat=${2:-.}
sudo -n sysctl -q debug.qwz.publish=1 2>/dev/null || true
ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | sed 's/^.*"QwzMacLog" = "//' | grep -E "$pat" | tail -n "$n"
