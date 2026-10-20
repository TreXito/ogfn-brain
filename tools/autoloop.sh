#!/usr/bin/env bash
# Self-driving check-and-report loop for the bot brain.
#
# Runs the tests, runs the behavioural bench over several seed ranges, checks the
# telemetry against human-plausible ranges, appends a report to logs/autoloop.log
# and prints the next thing worth working on.
#
# Default mode does NOT call any agent, so it is safe to run on a timer. Pass
# --engine "<cmd>" to also hand the brief to an agent CLI.
#
# Usage:
#   tools/autoloop.sh
#   tools/autoloop.sh --engine 'opencode run'
#   tools/autoloop.sh --lives 128 --seeds 4
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

LIVES=64
SEEDS=3
ENGINE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --engine) ENGINE="${2:-}"; shift 2 ;;
    --lives) LIVES="${2:-64}"; shift 2 ;;
    --seeds) SEEDS="${2:-3}"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

mkdir -p logs
LOG="logs/autoloop.log"
RUN_ID="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

report() { echo "$@" | tee -a "$LOG"; }

{
  echo
  echo "==================================================================="
  echo "autoloop run $RUN_ID  lives=$LIVES  seeds=$SEEDS"
  echo "==================================================================="
} >> "$LOG"

FAILED=0

# 0. Build everything the loop reads. Without this the loop runs whatever
# binaries happen to be lying in build/ and reports MISSING telemetry as a
# red bot model when the bench simply was never compiled.
if ! make -s all test-build bench-build > logs/build.out 2>&1; then
  tail -20 logs/build.out | tee -a "$LOG"
  report "BUILD FAILED"
  exit 1
fi

# 1. Unit and behavioural tests.
if ! make -s test > logs/test.out 2>&1; then
  tail -20 logs/test.out | tee -a "$LOG"
  report "TESTS FAILED"
  FAILED=1
else
  report "tests: $(grep -E 'checks, [0-9]+ failures' logs/test.out | tail -1)"
fi

# 2. Bench over several seed ranges, collecting telemetry.
BENCH_CLEAN=1
for s in $(seq 1 "$SEEDS"); do
  seed=$((s * 1000))
  if ! ./build/bench "$LIVES" 6 "$seed" > "logs/bench_$seed.txt" 2>&1; then
    BENCH_CLEAN=0
  fi
  grep -q "bench OK" "logs/bench_$seed.txt" || BENCH_CLEAN=0
done
report "bench across $SEEDS seed ranges: $([[ $BENCH_CLEAN == 1 ]] && echo OK || echo FAILED)"
[[ $BENCH_CLEAN == 1 ]] || FAILED=1

# 3. Human-plausibility check on the telemetry of the first range.
BENCH="logs/bench_1000.txt"
if [[ -f "$BENCH" ]]; then
  duty=$(grep -oP 'trigger duty: \K[0-9.]+' "$BENCH" | head -1)
  held=$(grep -oP 'held by reaction: \K[0-9.]+' "$BENCH" | head -1)
  repertoire=$(grep -oP 'bots with >=4 distinct actions: \K[0-9]+' "$BENCH" | head -1)
  buildps=$(grep -oP '\(\K[0-9.]+(?=/s per bot)' "$BENCH" | head -1)
  contact=$(grep -oP 'time in contact: \K[0-9.]+' "$BENCH" | head -1)
  buildhz=$(grep -oP 'building: \K[0-9.]+' "$BENCH" | head -1)
  edits=$(grep -oP 'edits \K[0-9]+' "$BENCH" | head -1)

  check() { # name value min max
    local name="$1" v="$2" lo="$3" hi="$4"
    if [[ -z "$v" ]]; then report "  $name: MISSING from bench output"; return 1; fi
    awk -v v="$v" -v lo="$lo" -v hi="$hi" 'BEGIN{exit !(v>=lo && v<=hi)}' \
      && report "  $name: $v (ok, want $lo..$hi)" \
      || { report "  $name: $v OUT OF RANGE (want $lo..$hi)"; return 1; }
  }

  report "human-plausibility:"
  check "trigger duty %" "$duty" 15 96 || FAILED=1
  check "reaction-held %" "$held" 0 8 || FAILED=1
  check "build inputs/s" "$buildps" 0.5 8 || FAILED=1
  check "time in contact %" "$contact" 5 70 || FAILED=1
  check "build pieces/s" "$buildhz" 0.02 2 || FAILED=1
  if [[ -n "$edits" && "$edits" -eq 0 ]]; then
    report "  piece edits: 0 (bots wall up and hide, they never edit a window)"
    FAILED=1
  else
    report "  piece edits: $edits (ok)"
  fi
  if [[ -n "$repertoire" && "$repertoire" -lt $((LIVES * 3 / 4)) ]]; then
    report "  repertoire: only $repertoire/$LIVES bots use 4+ actions: TOO NARROW"
    FAILED=1
  else
    report "  repertoire: $repertoire/$LIVES bots use 4+ actions (ok)"
  fi
fi

# 4. Decide the next work item and hand it over.
if [[ $FAILED == 1 ]]; then
  NEXT="The loop is RED. Reproduce with 'make test' and './build/bench 64 6 1000', fix the failing property, and re-run tools/autoloop.sh until it is green. Do not weaken a test to make it pass; fix the model."
else
  NEXT="The loop is green. Take the top unchecked item from README.md 'Next work, ranked', implement it behind tests the same way the existing layers are, then re-run tools/autoloop.sh."
fi

{
  echo
  echo "next: $NEXT"
  echo "telemetry: $(cat "$BENCH" 2>/dev/null)"
  echo
} >> "$LOG"

report ""
report "next: $NEXT"

if [[ -n "$ENGINE" ]]; then
  BRIEF="$ROOT/logs/brief.md"
  {
    echo "You are improving an offline battle royale bot brain in $ROOT/bots."
    echo "Read $ROOT/bots/README.md first, especially the human-ness contract."
    echo
    echo "Current status: $([[ $FAILED == 1 ]] && echo RED || echo green)."
    echo "$NEXT"
    echo
    echo "Rules: keep every behaviour pinned by a test, keep the bench green,"
    echo "never weaken a test to pass, and commit with a message that says what"
    echo "human tell you fixed."
  } > "$BRIEF"
  report "handing off to engine: $ENGINE"
  # shellcheck disable=SC2086
  $ENGINE "$(cat "$BRIEF")" 2>&1 | tee -a "$LOG"
fi

exit "$FAILED"
