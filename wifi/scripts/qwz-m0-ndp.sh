#!/bin/bash
mac=$1
IFS=: read a b c d e f <<< "$mac"
a=$(printf '%02x' $(( 0x$a ^ 2 )))
ll="fe80::${a}${b}:${c}ff:fe${d}:${e}${f}"
echo "neighbour entry $ll%awdl0 -> $mac"
sudo ndp -s "$ll%awdl0" "$mac"
ping6 -c 3 -I awdl0 "$ll"
