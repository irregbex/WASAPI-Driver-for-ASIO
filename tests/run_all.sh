#!/bin/sh
# WASAPI Driver for ASIO - builds and runs every offline test (Linux, plain g++). Exit code 0 only if ALL pass.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 0.9.8: the single list of offline tests, used by build.yml and by release.yml (a release is built only after this
# script succeeded) and by maintainers before every release (RELEASING.txt). Run from the source root or anywhere:
#   sh tests/run_all.sh            all tests (about 1 minute; fixedratio_test takes the longest)
#   SAN=0 sh tests/run_all.sh      the sanitizer-built tests without sanitizers (toolchains without libasan/libubsan)
# Each test prints one line per check and "RESULT: PASSED" / "RESULT: FAILED"; this script prints a summary table.
# Temporary binaries go to a fresh folder under ${TMPDIR:-/tmp} and are removed at the end.
set -u
cd "$(dirname "$0")/.."
OUT="$(mktemp -d "${TMPDIR:-/tmp}/wbtests.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
CXX="${CXX:-g++}"
BASE="-O2 -std=c++17 -Wall -Wextra -Werror"
SANFLAGS="-O1 -g -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -D_GLIBCXX_ASSERTIONS"
[ "${SAN:-1}" = "0" ] && SANFLAGS="-O1 -g -std=c++17 -Wall -Wextra -Werror -D_GLIBCXX_ASSERTIONS"
FAILED=0
SUMMARY=""
run() {   # run <name> <flags...>
    name="$1"; shift
    if ! $CXX "$@" "tests/$name.cpp" -o "$OUT/$name" > "$OUT/$name.build" 2>&1; then
        cat "$OUT/$name.build"; SUMMARY="$SUMMARY
  $name: BUILD FAILED"; FAILED=1; return
    fi
    if "$OUT/$name" > "$OUT/$name.log" 2>&1; then
        SUMMARY="$SUMMARY
  $name: passed"
    else
        cat "$OUT/$name.log"; SUMMARY="$SUMMARY
  $name: FAILED"; FAILED=1
    fi
}
run resampler_test   $BASE
run sink_test        $BASE
run dmix_test        $BASE
run live_test        $BASE -pthread
run limiter_test     $BASE
run chnames_test     $BASE
run visuals_test     $BASE -D_GLIBCXX_ASSERTIONS
run timeinfo_test    $BASE -pthread
run strings_test     $BASE
run themes_test      $BASE
run update_test      $BASE
run registry_test    $BASE -pthread
run icon_test        $BASE
run fixedratio_test  $BASE
run ratepolicy_test  $BASE
# 0.9.8 (hardening): fuzzing of every parser of untrusted data, the host-argument validation and the trace encoder,
# built with AddressSanitizer + UndefinedBehaviorSanitizer + libstdc++ assertions
run fuzz_test        $SANFLAGS
run asiocheck_test   $SANFLAGS
run trace_test       $SANFLAGS
echo "Offline tests:$SUMMARY"
if [ "$FAILED" -ne 0 ]; then echo "RESULT: FAILED"; exit 1; fi
echo "RESULT: ALL PASSED"
