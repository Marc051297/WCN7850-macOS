#!/bin/sh
set -e
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/awdl_cell_test.$$
clang -std=c11 -Wall -Wextra -Wno-unused-parameter -I qwz -o "$OUT" tests/awdl_cell_test.c qwz/qwz_awdl_cell.c
"$OUT"
rm -f "$OUT"
