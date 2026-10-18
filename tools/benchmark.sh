#!/usr/bin/env bash
# Fly a benchmark route with the client and print its summary and report.
#
#   tools/benchmark.sh [route] [seconds]      (default: dalaran 60)
#
# Routes: dalaran, stormwind, orgrimmar (src/core/benchmark_run.cpp). The
# client enters the route's map offline - no realm, no login - waits for the
# world to settle, flies one lap in the given seconds, writes
# logs/benchmark-<route>-<stamp>.txt beside itself and closes. See
# docs/perf_baseline.md for what the report holds.
#
# Runs build-dev/bin/wowee.exe (WOWEE_BIN overrides) from its own folder, so
# its logs land on the same drive; the game data is WOW_DATA_PATH, or Data/
# beside this repository. WOWEE_BENCHMARK_SETTLE sets the least settle time
# (10 s). Close other clients first: they share the CPU and the GPU, and a run
# beside one is not comparable with one without.
set -euo pipefail

route="${1:-dalaran}"
seconds="${2:-60}"
root="$(cd "$(dirname "$0")/.." && pwd)"
bin="${WOWEE_BIN:-$root/build-dev/bin/wowee.exe}"
mkdir -p "$root/build-dev/tmp"

to_native() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

export TMP="$(to_native "$root/build-dev/tmp")" TEMP="$(to_native "$root/build-dev/tmp")"
export WOW_DATA_PATH="${WOW_DATA_PATH:-$(to_native "$root/Data")}"
export WOWEE_BENCHMARK="$route" WOWEE_BENCHMARK_SECONDS="$seconds"

cd "$(dirname "$bin")"
log="logs/benchmark-run.log"
mkdir -p logs
started="logs/.benchmark-started"
touch "$started"
WOWEE_LOG_FILE="benchmark-run.log" "./$(basename "$bin")" > /dev/null 2>&1 || true
# The report file itself, not the log's line about it: the log's last lines
# can still be in the logger's buffer when the client exits.
report="$(find logs -maxdepth 1 -name "benchmark-$route-*.txt" -newer "$started" | sort | tail -1)"
rm -f "$started"
if [[ -z "$report" ]]; then
    echo "benchmark: no report - the end of $(pwd)/$log:" >&2
    tail -20 "$log" >&2
    exit 1
fi
grep "^summary:" "$report"
echo "report: $(pwd)/$report"
