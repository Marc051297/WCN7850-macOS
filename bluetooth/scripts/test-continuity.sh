#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
xcrun clang++ -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I compat tests/qca_hci.cpp -o build/tests/qca_hci
build/tests/qca_hci
xcrun clang++ -std=c++14 -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
  -I compat -I tests tests/qca_async.cpp -o build/tests/qca_async
build/tests/qca_async
python3 -I -m py_compile scripts/prepare-deps.py scripts/apply-compat.py tools/check-continuity.py
