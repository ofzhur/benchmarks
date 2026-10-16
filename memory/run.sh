#!/usr/bin/env bash
# memory/run.sh - memory bandwidth (read/write/copy/triad) swept over thread
# counts, and load-to-use latency of every cache level and DRAM.
#
# Author: Oleg Zhurko <oleg@zhurko.de>
# SPDX-License-Identifier: MIT
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
. "$HERE/../lib/common.sh"
# shellcheck source=../lib/hw.sh
. "$HERE/../lib/hw.sh"

usage() {
  cat <<EOF
Usage: memory/run.sh [options]

Bandwidth: arrays of at least 4x the total last-level cache, NUMA-local
first touch, threads spread over NUMA nodes and cores, every kernel tried
with SIMD loops, non-temporal stores and libc memset/memcpy; the thread
count is swept and the best result is reported.
Latency: random pointer chase from 4 KiB up to 4x LLC (at least 512 MiB).

Options:
  -s, --size SIZE      size of each of the 3 arrays (default: auto)
  -T, --threads LIST   thread counts, e.g. 1,8,16 (default: auto sweep)
  -r, --repeat N       repetitions per point, best kept (default: 5, quick 2, long 10)
      --no-latency     bandwidth only
      --latency-only   latency only
$COMMON_OPTIONS_HELP
EOF
}

opt_size="" opt_threads="auto" opt_repeat="" opt_bw=1 opt_lat=1
while (( $# )); do
  case $1 in
    -s|--size) req_arg "$1" $#; opt_size=$2; shift 2 ;;
    -T|--threads) req_arg "$1" $#; opt_threads=$2; shift 2 ;;
    -r|--repeat) req_arg "$1" $#; opt_repeat=$2; shift 2 ;;
    --no-latency) opt_lat=0; shift ;;
    --latency-only) opt_bw=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) n=0; common_opt "$@" || n=$?; (( n )) || die "unknown option: $1 (see --help)"; shift "$n" ;;
  esac
done
(( opt_bw || opt_lat )) || die "nothing to do"

suite_begin memory "Memory"
bin=$(build_tool membench "$HERE/membench.c") || exit 1

args=(--kv --threads "$opt_threads" --repeat "${opt_repeat:-$(pick 2 5 10)}"
      --lat-time "$(pick 0.03 0.1 0.3)")
[[ -n $opt_size ]] && args+=(--size "$opt_size")
if (( opt_bw && opt_lat )); then args+=(-L)
elif (( opt_lat )); then args+=(--latency-only)
fi

log="$RAW_DIR/membench.log"
info "membench ${args[*]}"
"$bin" "${args[@]}" 2>&1 | tee "$log" | grep -v '^RESULT ' | sed 's/^/    /'
(( PIPESTATUS[0] == 0 )) || die "membench failed (see $log)"

while read -r line; do
  case $(kv_get "$line" kind) in
    bw)
      result memory "$(kv_get "$line" kernel)_bw" "$(kv_get "$line" value)" "GB/s" \
        "threads=$(kv_get "$line" threads) method=$(kv_get "$line" method)" ;;
    lat)
      result memory "latency_$(kv_get "$line" level)" "$(kv_get "$line" value)" "ns" \
        "working_set=$(human_bytes "$(kv_get "$line" bytes)" | tr -d ' ')" ;;
  esac
done < <(grep '^RESULT ' "$log")

suite_end memory
