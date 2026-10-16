#!/usr/bin/env bash
# cpu/run.sh - CPU benchmark: integer ALU, FP64/FP32 FMA peak, SHA-256 and
# sort on one thread and on every CPU, plus well-known reference tools
# (sysbench, 7-Zip, OpenSSL) when they are installed.
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
Usage: cpu/run.sh [options]

Runs cpubench (built with -march=native for this CPU) once on a single
pinned thread and once with one pinned thread per CPU. The FMA kernels are
auto-selected (vector width x accumulators) to reach the hardware peak.

Options:
  -t, --tests LIST     int,fp64,fp32,sha256,sort (default: all)
  -T, --threads N      threads of the multi-thread run (default: all CPUs)
  -d, --duration SEC   seconds per test and mode (default: 5, quick 2, long 10)
      --no-extras      skip sysbench / 7-Zip / OpenSSL reference runs
$COMMON_OPTIONS_HELP
EOF
}

opt_tests="" opt_threads="" opt_duration="" opt_extras=1
while (( $# )); do
  case $1 in
    -t|--tests) req_arg "$1" $#; opt_tests=$2; shift 2 ;;
    -T|--threads) req_arg "$1" $#; opt_threads=$2; shift 2 ;;
    -d|--duration) req_arg "$1" $#; opt_duration=$2; shift 2 ;;
    --no-extras) opt_extras=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) n=0; common_opt "$@" || n=$?; (( n )) || die "unknown option: $1 (see --help)"; shift "$n" ;;
  esac
done

suite_begin cpu "CPU"
bin=$(build_tool cpubench "$HERE/cpubench.c") || exit 1
dur=${opt_duration:-$(ptime 5 2 10)}
ncpu=$(hw_ncpus)
threads=${opt_threads:-$ncpu}

args=(--duration "$dur" --kv)
[[ -n $opt_tests ]] && args+=(--tests "$opt_tests")
[[ -n $opt_threads ]] && args+=(--threads "$opt_threads")
log="$RAW_DIR/cpubench.log"
info "cpubench: ${dur}s per test and mode"
"$bin" "${args[@]}" 2>&1 | tee "$log" | grep -v '^RESULT ' | sed 's/^/    /'
(( PIPESTATUS[0] == 0 )) || die "cpubench failed (see $log)"

while read -r line; do
  t=$(kv_get "$line" test) mode=$(kv_get "$line" mode) v=$(kv_get "$line" value)
  u=$(kv_get "$line" unit) th=$(kv_get "$line" threads) k=$(kv_get "$line" kernel)
  result cpu "${t}_${mode}" "$v" "$u" "threads=$th kernel=$k"
done < <(grep '^RESULT ' "$log")

# ---------------------------------------------------------------- extras
if (( opt_extras )) && [[ $BENCH_PROFILE != quick ]]; then
  if have sysbench; then
    info "sysbench cpu (primes up to 10000)"
    for t in 1 "$threads"; do
      out=$(sysbench cpu --threads="$t" --time="$dur" run 2>&1)
      printf '%s\n' "$out" > "$RAW_DIR/sysbench-cpu-${t}t.log"
      eps=$(awk '/events per second/ { print $NF }' <<< "$out")
      if [[ -n $eps ]]; then
        mode=$([[ $t == 1 ]] && echo 1t || echo mt)
        step "$t thread(s): $eps events/s"
        result cpu "sysbench_$mode" "$eps" "events/s" "threads=$t"
      fi
    done
  fi

  sz=""
  for c in 7zz 7z 7za; do have "$c" && { sz=$c; break; }; done
  if [[ -n $sz ]]; then
    info "7-Zip compression benchmark ($sz b)"
    for t in 1 "$threads"; do
      out=$("$sz" b "-mmt$t" 2>&1)
      printf '%s\n' "$out" > "$RAW_DIR/7zip-${t}t.log"
      mips=$(awk '/^Tot:/ { print $NF }' <<< "$out")
      if [[ -n $mips ]]; then
        mode=$([[ $t == 1 ]] && echo 1t || echo mt)
        step "$t thread(s): $mips MIPS"
        result cpu "7zip_$mode" "$mips" "MIPS" "threads=$t"
      fi
    done
  fi

  if have openssl; then
    info "OpenSSL speed (16 KiB blocks, hardware acceleration allowed)"
    for alg in aes-256-gcm sha256; do
      for t in 1 "$threads"; do
        multi=()
        (( t > 1 )) && multi=(-multi "$t")
        out=$(openssl speed -elapsed -seconds "$dur" -bytes 16384 -mr ${multi[@]+"${multi[@]}"} -evp "$alg" 2>&1)
        printf '%s\n' "$out" > "$RAW_DIR/openssl-$alg-${t}t.log"
        bps=$(awk -F: '/^\+F:/ { v = $NF } END { if (v != "") print v }' <<< "$out")
        if [[ -n $bps ]]; then
          mode=$([[ $t == 1 ]] && echo 1t || echo mt)
          mbs=$(fdiv "$bps" 1000000 1)
          step "$alg, $t thread(s): $mbs MB/s"
          result cpu "openssl_${alg//-/_}_$mode" "$mbs" "MB/s" "threads=$t"
        else
          step "openssl speed $alg not supported by this OpenSSL version"
          break
        fi
      done
    done
  fi
fi

suite_end cpu
