#!/usr/bin/env bash
# bench.sh - runs the benchmark suites and collects the results in one
# directory (results.json, summary.md, bench.log and the raw tool output).
#
# Author: Oleg Zhurko <oleg@zhurko.de>
# SPDX-License-Identifier: MIT
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
. "$HERE/lib/common.sh"
# shellcheck source=lib/hw.sh
. "$HERE/lib/hw.sh"

ALL_SUITES=(cpu memory disk network http)

usage() {
  cat <<EOF
Usage: ./bench.sh [options] [suite ...]

Suites: sysinfo ${ALL_SUITES[*]}   (default: all of them)

Every suite sizes and tunes itself for the machine it runs on (CPU count and
topology, SIMD width, cache and RAM sizes, storage and network devices) to
measure the highest performance the hardware reaches on this OS.

Options:
  --quick | --long | --profile P   quick (~6 min), normal (~15-25 min, default) or long (~1 h)
  -o, --output DIR                 results directory (default: results/<date>-<host>)
  --disk-dir DIR                   directory whose file system the disk suite tests
                                   (default: the current directory)
  --net-host HOST                  network suite against 'network/run.sh server' on HOST
                                   (default: loopback, i.e. the local TCP stack)
  --http-url URL                   http suite against 'http/run.sh server' at URL
                                   (default: server and load generator on this machine)
  --tune                           set the 'performance' CPU governor for the run and
                                   restore the previous one afterwards (root)
  --install-deps                   install the required packages first (sudo if not root)
  --list                           list the suites
  --no-color                       plain output
  -h, --help                       this help

Each suite also runs on its own with more options, e.g. './disk/run.sh --help'.
EOF
}

suites=() opt_disk_dir="" opt_net_host="" opt_http_url="" opt_tune=0 opt_install=0
argv=("$@")
while (( $# )); do
  case $1 in
    --disk-dir) req_arg "$1" $#; opt_disk_dir=$2; shift 2 ;;
    --net-host) req_arg "$1" $#; opt_net_host=$2; shift 2 ;;
    --http-url) req_arg "$1" $#; opt_http_url=$2; shift 2 ;;
    --tune) opt_tune=1; shift ;;
    --install-deps) opt_install=1; shift ;;
    --list) echo "sysinfo ${ALL_SUITES[*]}"; exit 0 ;;
    -h|--help) usage; exit 0 ;;
    all) suites+=(sysinfo "${ALL_SUITES[@]}"); shift ;;
    sysinfo|cpu|memory|disk|network|http) suites+=("$1"); shift ;;
    -*) n=0; common_opt "$@" || n=$?; (( n )) || die "unknown option: $1 (see --help)"; shift "$n" ;;
    *) die "unknown suite '$1' (available: sysinfo ${ALL_SUITES[*]})" ;;
  esac
done
(( ${#suites[@]} )) || suites=(sysinfo "${ALL_SUITES[@]}")

# Re-run under tee so that everything printed by every suite also lands in
# bench.log (colors are kept on the terminal, stripped from the log).
if [[ -z "${_BENCH_LOGGING:-}" ]]; then
  if (( opt_install )); then install_deps || exit 1; fi
  results_init
  if _bench_use_color; then export BENCH_COLOR=1; else export BENCH_COLOR=0; fi
  export _BENCH_LOGGING=1 BENCH_PROFILE
  log="$BENCH_RESULTS_DIR/bench.log"
  "$HERE/bench.sh" ${argv[@]+"${argv[@]}"} 2>&1 | tee -a "$log"
  rc=${PIPESTATUS[0]}
  sed -i $'s/\033\\[[0-9;]*m//g' "$log" 2>/dev/null
  exit "$rc"
fi

results_init

tune_governor() {
  local f saved="$RAW_DIR/governor.saved"
  if (( EUID != 0 )); then warn "--tune needs root: CPU governor left unchanged"; return; fi
  : > "$saved"
  for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    [[ -w $f ]] || continue
    printf '%s %s\n' "$f" "$(cat "$f")" >> "$saved"
    echo performance > "$f" 2>/dev/null || true
  done
  if [[ -s $saved ]]; then
    on_exit "while read -r f g; do echo \"\$g\" > \"\$f\" 2>/dev/null; done < '$saved'"
    info "CPU frequency governor set to 'performance' (restored when the run ends)"
  else
    step "no cpufreq governor to tune on this system"
  fi
}

header "Benchmark suite $BENCH_VERSION"
info "results: $BENCH_RESULTS_DIR"
info "profile: $BENCH_PROFILE, suites: ${suites[*]}"
(( opt_tune )) && tune_governor

header "System"
sysinfo_collect
sysinfo_warnings

declare -A status elapsed
for suite in "${suites[@]}"; do
  [[ $suite == sysinfo ]] && continue
  [[ -n ${status[$suite]:-} ]] && continue # listed twice
  args=()
  case $suite in
    disk) [[ -n $opt_disk_dir ]] && args=(--dir "$opt_disk_dir") ;;
    network) [[ -n $opt_net_host ]] && args=(client "$opt_net_host") ;;
    http) [[ -n $opt_http_url ]] && args=(client "$opt_http_url") ;;
  esac
  t0=$(date +%s)
  "$HERE/$suite/run.sh" ${args[@]+"${args[@]}"}
  rc=$?
  elapsed[$suite]=$(( $(date +%s) - t0 ))
  case $rc in
    0) status[$suite]=ok ;;
    "$EXIT_SKIPPED") status[$suite]=skipped ;;
    130|143) warn "interrupted"; exit "$rc" ;;
    *) status[$suite]="failed ($rc)"; warn "suite '$suite' failed with exit code $rc" ;;
  esac
done

header "Summary"
for suite in "${suites[@]}"; do
  [[ $suite == sysinfo || -z ${status[$suite]:-} ]] && continue
  printf '  %-8s %-12s %ss\n' "$suite" "${status[$suite]}" "${elapsed[$suite]}"
done
echo
print_results
results_finalize
echo
info "reports: $BENCH_RESULTS_DIR/summary.md and results.json"
info "log and raw tool output: $BENCH_RESULTS_DIR/bench.log, raw/"
