#!/bin/zsh
out=${1:-$HOME/qwz-stream-m0.log}
while true; do
  ssh -t -o ServerAliveInterval=2 -o ServerAliveCountMax=3 user@hackintosh.local 'echo "$(date +%T) window A: type the hackintosh password for sudo now (nothing is shown while you type)"; sudo -v || { echo "$(date +%T) window A: sudo did not get a password, reconnecting in 2 s"; exit 1; }; echo "$(date +%T) window A: streaming"; while true; do sudo sysctl -q debug.qwz.publish=1; ioreg -l -w0 -r -k QwzMacLog | sed -n "/\"QwzMacLog\" = \"/,/^\"\$/p" | sed "s/^.*\"QwzMacLog\" = \"//"; ioreg -l -w0 -r -k QwzMacCrumb | grep QwzMacCrumb; sleep 1; done' | awk '!seen[$0]++ { print; fflush() }' | tee -a "$out"
  sleep 2
done
