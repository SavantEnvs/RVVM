#!/usr/bin/env bash
#
# mayhem/test.sh — RUN the known-answer selftest that mayhem/build.sh built (RVVM ships no
# test suite of its own). exit 0 = pass. PATCH-grade oracle: after an agent patches the source,
# the grader rebuilds (build.sh) then runs this.
#
# mayhem/selftest/rvvm_selftest.c boots RV64 and RV32 guests on the interpreter and on RVJIT,
# checks the values a known guest program leaves in RAM (incl. signed division overflow), and
# checks the userland NAT stack answers an ARP request. Each check prints PASS/FAIL; the
# final SUMMARY line must be present, so a binary that does nothing fails.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

RUNNER="$SRC/mayhem/out/rvvm_selftest"
if [ ! -x "$RUNNER" ]; then
  echo "test.sh: $RUNNER missing — run mayhem/build.sh first" >&2
  emit_ctrf "rvvm-selftest" 0 1 0
  exit 1
fi
EXPECTED=59   # checks in rvvm_selftest.c
log=$(cd "${TMPDIR:-/tmp}" && timeout 300 "$RUNNER" 2>&1)
rc=$?
echo "$log"
passed=$(grep -c '^PASS ' <<<"$log")
failed=$(grep -c '^FAIL ' <<<"$log")
if ! grep -q "^SUMMARY passed=$passed failed=$failed\$" <<<"$log"; then
  echo "test.sh: selftest summary missing or inconsistent (rc=$rc)" >&2
  failed=$((failed + 1))
elif [ $((passed + failed)) -ne "$EXPECTED" ]; then
  echo "test.sh: expected $EXPECTED checks, saw $((passed + failed))" >&2
  failed=$((failed + EXPECTED - passed - failed))
  [ "$failed" -gt 0 ] || failed=1
elif [ "$rc" -ne 0 ] && [ "$failed" -eq 0 ]; then
  failed=1
fi
emit_ctrf "rvvm-selftest" "$passed" "$failed" 0
