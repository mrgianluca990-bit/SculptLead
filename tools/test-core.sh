#!/usr/bin/env bash
set -euo pipefail
sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_output="${SCULPT_LEAD_TEST_DIR:-${sculpt_root}/build-core}"
mkdir -p "$sculpt_output"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -I "${sculpt_root}/Source" \
  "${sculpt_root}/Tests/lead_engine_test.cpp" \
  -o "${sculpt_output}/lead-engine-test"
"${sculpt_output}/lead-engine-test"
