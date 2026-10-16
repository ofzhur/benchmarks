#!/usr/bin/env bash
# shellcheck shell=bash
#
# lib/common.sh - shared helpers for all benchmark suites: output, options,
# run profiles, results, dependency checks and building the C tools.
#
# Author: Oleg Zhurko <oleg@zhurko.de>
# SPDX-License-Identifier: MIT

[[ -n "${_BENCH_COMMON_SH:-}" ]] && return 0
_BENCH_COMMON_SH=1

BENCH_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_VERSION="1.0.0"
BENCH_PROFILE="${BENCH_PROFILE:-normal}"
export BENCH_ROOT BENCH_PROFILE

# ------------------------------------------------------------------ output

_bench_use_color() {
  [[ -n "${NO_COLOR:-}" ]] && return 1
  if [[ -n "${BENCH_COLOR:-}" ]]; then [[ $BENCH_COLOR == 1 ]]; return; fi
  [[ -t 1 ]]
}

_bench_set_colors() {
  if _bench_use_color; then
    C_RESET=$'\033[0m' C_BOLD=$'\033[1m' C_DIM=$'\033[2m' C_RED=$'\033[31m'
    C_GREEN=$'\033[32m' C_YELLOW=$'\033[33m' C_BLUE=$'\033[34m' C_CYAN=$'\033[36m'
  else
    C_RESET='' C_BOLD='' C_DIM='' C_RED='' C_GREEN='' C_YELLOW='' C_BLUE='' C_CYAN=''
  fi
}
_bench_set_colors

header() { printf '\n%s%s=== %s ===%s\n' "$C_BOLD" "$C_CYAN" "$*" "$C_RESET"; }
info()   { printf '%s==>%s %s\n' "$C_BOLD$C_BLUE" "$C_RESET" "$*"; }
step()   { printf '    %s%s%s\n' "$C_DIM" "$*" "$C_RESET"; }
good()   { printf '    %s%s%s\n' "$C_GREEN" "$*" "$C_RESET"; }
warn()   { printf '%sWARNING:%s %s\n' "$C_BOLD$C_YELLOW" "$C_RESET" "$*" >&2; }
die()    { printf '%sERROR:%s %s\n' "$C_BOLD$C_RED" "$C_RESET" "$*" >&2; exit 1; }

# Suites exit with this status when a required tool is missing, so that
# bench.sh can report them as skipped rather than failed.
EXIT_SKIPPED=3
skip() { warn "$*"; exit "$EXIT_SKIPPED"; }

# --------------------------------------------------------------- cleanup

_BENCH_EXIT_HOOKS=()
on_exit() { _BENCH_EXIT_HOOKS+=("$*"); }
_bench_run_exit_hooks() {
  local i
  for (( i = ${#_BENCH_EXIT_HOOKS[@]} - 1; i >= 0; i-- )); do
    eval "${_BENCH_EXIT_HOOKS[i]}" || true
  done
  _BENCH_EXIT_HOOKS=()
}
trap _bench_run_exit_hooks EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# --------------------------------------------------------------- options

set_profile() {
  case $1 in
    quick|normal|long) BENCH_PROFILE=$1; export BENCH_PROFILE ;;
    *) die "unknown profile '$1' (use quick, normal or long)" ;;
  esac
}

# req_arg OPTION ARGC - dies unless the option has a value
req_arg() { (( $2 >= 2 )) || die "option $1 needs a value"; }

# Handles options shared by every suite. Returns the number of consumed
# arguments (0 = not a common option):
#   n=0; common_opt "$@" || n=$?; (( n )) || die "unknown option"; shift "$n"
common_opt() {
  case $1 in
    --quick) set_profile quick; return 1 ;;
    --long) set_profile long; return 1 ;;
    --profile) req_arg "$1" $#; set_profile "$2"; return 2 ;;
    --profile=*) set_profile "${1#*=}"; return 1 ;;
    -o|--output) req_arg "$1" $#; BENCH_RESULTS_DIR=$2; export BENCH_RESULTS_DIR; return 2 ;;
    --output=*) BENCH_RESULTS_DIR=${1#*=}; export BENCH_RESULTS_DIR; return 1 ;;
    --no-color) NO_COLOR=1; export NO_COLOR; _bench_set_colors; return 1 ;;
  esac
  return 0
}

# shellcheck disable=SC2034 # used in the usage text of every suite
COMMON_OPTIONS_HELP="Common options:
  --quick | --long | --profile P   run profile: quick, normal (default), long
  -o, --output DIR                 results directory (default: results/<date>-<host>)
  --no-color                       plain output
  -h, --help                       this help"

# ptime NORMAL [QUICK] [LONG] - a duration in seconds for the active profile
ptime() {
  local n=$1 q=${2:-} l=${3:-}
  case $BENCH_PROFILE in
    quick) [[ -n $q ]] || q=$(( (n + 2) / 3 )); (( q < 1 )) && q=1; echo "$q" ;;
    long) echo "${l:-$(( n * 3 ))}" ;;
    *) echo "$n" ;;
  esac
}

# pick QUICK NORMAL LONG - any value for the active profile
pick() {
  case $BENCH_PROFILE in
    quick) echo "$1" ;;
    long) echo "$3" ;;
    *) echo "$2" ;;
  esac
}

# --------------------------------------------------------------- results

short_hostname() { local h; h=$(uname -n 2>/dev/null); echo "${h%%.*}"; }

results_init() {
  if [[ -z "${BENCH_RESULTS_DIR:-}" ]]; then
    BENCH_RESULTS_DIR="$BENCH_ROOT/results/$(date +%Y%m%d-%H%M%S)-$(short_hostname)"
  fi
  mkdir -p "$BENCH_RESULTS_DIR/raw" || die "cannot create $BENCH_RESULTS_DIR"
  BENCH_RESULTS_DIR=$(cd "$BENCH_RESULTS_DIR" && pwd)
  export BENCH_RESULTS_DIR
  RESULTS_TSV="$BENCH_RESULTS_DIR/results.tsv"
  SYSINFO_TSV="$BENCH_RESULTS_DIR/sysinfo.tsv"
  # shellcheck disable=SC2034 # raw tool output, used by the suites
  RAW_DIR="$BENCH_RESULTS_DIR/raw"
  touch "$RESULTS_TSV" "$SYSINFO_TSV"
}

# result SUITE TEST VALUE UNIT [DETAILS] - records one measurement
result() {
  local d=${5:-}
  d=${d//$'\t'/ }
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$d" >> "$RESULTS_TSV"
}

# sysinfo_set KEY VALUE - records (or replaces) one system property
sysinfo_set() {
  local v=${2//$'\t'/ }
  v=${v//$'\n'/ }
  K=$1 V=$v awk -F'\t' 'BEGIN { OFS = "\t"; k = ENVIRON["K"]; v = ENVIRON["V"] }
    $1 == k { $0 = k OFS v; f = 1 } { print } END { if (!f) print k, v }' \
    "$SYSINFO_TSV" > "$SYSINFO_TSV.tmp" && mv "$SYSINFO_TSV.tmp" "$SYSINFO_TSV"
}

sysinfo_get() { K=$1 awk -F'\t' '$1 == ENVIRON["K"] { print $2; exit }' "$SYSINFO_TSV"; }

# print_results [SUITE] - aligned table of the recorded results
print_results() {
  [[ -s "$RESULTS_TSV" ]] || { step "(no results recorded)"; return 0; }
  S=${1:-} awk -F'\t' '
    function commas(x,   s, neg) {
      neg = (x ~ /^-/); sub(/^-/, "", x); s = ""
      while (length(x) > 3) { s = "," substr(x, length(x) - 2) s; x = substr(x, 1, length(x) - 3) }
      return (neg ? "-" : "") x s
    }
    function fmt(v,   a) {
      if (v !~ /^-?[0-9]*\.?[0-9]+([eE][-+]?[0-9]+)?$/) return v
      a = v < 0 ? -v : v
      if (a >= 1000) return commas(sprintf("%.0f", v))
      if (a >= 100) return sprintf("%.1f", v)
      if (a >= 1) return sprintf("%.2f", v)
      return sprintf("%.3f", v)
    }
    BEGIN { s = ENVIRON["S"] }
    s == "" || $1 == s { printf "  %-8s %-24s %14s  %-8s %s\n", $1, $2, fmt($3), $4, $5 }
  ' "$RESULTS_TSV"
}

# results_finalize - writes results.json and summary.md next to the TSV files
results_finalize() {
  local json="$BENCH_RESULTS_DIR/results.json" md="$BENCH_RESULTS_DIR/summary.md"
  VER=$BENCH_VERSION PROF=$BENCH_PROFILE awk -F'\t' '
    function esc(s) { gsub(/\\/, "\\\\", s); gsub(/"/, "\\\"", s); gsub(/\r/, "", s); return s }
    function val(s) {
      return (s ~ /^-?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?$/) ? s : "\"" esc(s) "\""
    }
    FILENAME == ARGV[1] { if ($1 != "") sys[++ns] = sprintf("\"%s\": %s", esc($1), val($2)); next }
    $1 != "" {
      res[++nr] = sprintf("{\"suite\": \"%s\", \"test\": \"%s\", \"value\": %s, \"unit\": \"%s\", \"details\": \"%s\"}",
                          esc($1), esc($2), val($3), esc($4), esc($5))
    }
    END {
      printf "{\n  \"suite_version\": \"%s\",\n  \"profile\": \"%s\",\n  \"system\": {\n", ENVIRON["VER"], ENVIRON["PROF"]
      for (i = 1; i <= ns; i++) printf "    %s%s\n", sys[i], (i < ns ? "," : "")
      printf "  },\n  \"results\": [\n"
      for (i = 1; i <= nr; i++) printf "    %s%s\n", res[i], (i < nr ? "," : "")
      printf "  ]\n}\n"
    }' "$SYSINFO_TSV" "$RESULTS_TSV" > "$json"
  {
    printf '# Benchmark results: %s\n\n' "$(sysinfo_get hostname)"
    printf 'Profile `%s`, suite version %s.\n\n' "$BENCH_PROFILE" "$BENCH_VERSION"
    printf '| System | |\n|---|---|\n'
    awk -F'\t' '$2 != "" { gsub(/\|/, "\\|"); printf "| %s | %s |\n", $1, $2 }' "$SYSINFO_TSV"
    printf '\n| Suite | Test | Value | Unit | Details |\n|---|---|--:|---|---|\n'
    awk -F'\t' '{ gsub(/\|/, "\\|"); printf "| %s | %s | %s | %s | %s |\n", $1, $2, $3, $4, $5 }' "$RESULTS_TSV"
  } > "$md"
}

# ------------------------------------------------------------ dependencies

have() { command -v "$1" >/dev/null 2>&1; }

pkg_manager() {
  local pm
  for pm in apt-get dnf yum zypper pacman apk; do
    have "$pm" && { echo "$pm"; return 0; }
  done
  return 1
}

# package that provides a command on this distribution
pkg_for() {
  local pm
  pm=$(pkg_manager) || { echo "$1"; return; }
  case "$1:$pm" in
    gcc:apt-get|cc:apt-get) echo build-essential ;;
    gcc:apk|cc:apk) echo build-base ;;
    cc:*) echo gcc ;;
    iperf3:zypper) echo iperf ;;
    taskset:*|lsblk:*|findmnt:*|lscpu:*) echo util-linux ;;
    ip:apt-get|ip:zypper|ip:pacman|ip:apk) echo iproute2 ;;
    ip:*) echo iproute ;;
    ping:apt-get) echo iputils-ping ;;
    ping:*) echo iputils ;;
    7z:apt-get) echo p7zip-full ;;
    *) echo "$1" ;;
  esac
}

install_hint() {
  local pkg
  pkg=$(pkg_for "$1")
  case $(pkg_manager) in
    apt-get) echo "sudo apt-get install -y $pkg" ;;
    dnf) echo "sudo dnf install -y $pkg" ;;
    yum) echo "sudo yum install -y $pkg" ;;
    zypper) echo "sudo zypper install -y $pkg" ;;
    pacman) echo "sudo pacman -S $pkg" ;;
    apk) echo "sudo apk add $pkg" ;;
    *) echo "your package manager (package: $pkg)" ;;
  esac
}

# need CMD - true if CMD exists, otherwise warns with an install hint
need() {
  have "$1" && return 0
  warn "'$1' not found. Install it with: $(install_hint "$1")   (or: ./bench.sh --install-deps)"
  return 1
}

# Installs everything the suites use. Optional tools are installed one by
# one so that a package missing from some distribution does not stop the rest.
install_deps() {
  local pm sudo=() required=() optional=() p
  pm=$(pkg_manager) || die "no supported package manager (apt, dnf, yum, zypper, pacman, apk)"
  if (( EUID != 0 )); then
    have sudo || die "run as root or install sudo"
    sudo=(sudo)
  fi
  case $pm in
    apt-get)
      required=(build-essential fio iperf3 util-linux procps iproute2 iputils-ping)
      optional=(wrk sysbench p7zip-full numactl dmidecode ethtool) ;;
    dnf|yum)
      required=(gcc make fio iperf3 util-linux procps-ng iproute iputils)
      optional=(wrk sysbench p7zip 7zip numactl dmidecode ethtool) ;;
    zypper)
      required=(gcc make fio iperf util-linux procps iproute2 iputils)
      optional=(wrk sysbench 7zip p7zip numactl dmidecode ethtool) ;;
    pacman)
      required=(gcc make fio iperf3 util-linux procps-ng iproute2 iputils)
      optional=(wrk sysbench 7zip numactl dmidecode ethtool) ;;
    apk)
      required=(bash build-base linux-headers fio iperf3 util-linux procps iproute2 iputils coreutils)
      optional=(wrk sysbench 7zip numactl dmidecode ethtool) ;;
  esac
  info "installing with $pm: ${required[*]}"
  _pm_install() {
    case $pm in
      apt-get) ${sudo[@]+"${sudo[@]}"} env DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$@" ;;
      dnf|yum) ${sudo[@]+"${sudo[@]}"} "$pm" install -y "$@" ;;
      zypper) ${sudo[@]+"${sudo[@]}"} zypper --non-interactive install "$@" ;;
      pacman) ${sudo[@]+"${sudo[@]}"} pacman -S --needed --noconfirm "$@" ;;
      apk) ${sudo[@]+"${sudo[@]}"} apk add --no-cache "$@" ;;
    esac
  }
  case $pm in
    apt-get) ${sudo[@]+"${sudo[@]}"} env DEBIAN_FRONTEND=noninteractive apt-get update -q || warn "apt-get update failed" ;;
    pacman) ${sudo[@]+"${sudo[@]}"} pacman -Sy --noconfirm || warn "pacman -Sy failed" ;;
  esac
  _pm_install "${required[@]}" || die "installing required packages failed"
  for p in "${optional[@]}"; do
    _pm_install "$p" >/dev/null 2>&1 && step "installed optional $p" || step "optional $p not available"
  done
  good "dependencies installed"
}

# ----------------------------------------------------------------- build

BUILD_DIR="${BENCH_BUILD_DIR:-$BENCH_ROOT/build/$(uname -m)-$(short_hostname)}"

find_cc() {
  local c
  for c in "${CC:-}" cc gcc clang; do
    [[ -n $c ]] && have "$c" && { echo "$c"; return 0; }
  done
  return 1
}

cc_version() { local cc; cc=$(find_cc) && "$cc" --version 2>/dev/null | head -1; }

_cc_accepts() {
  local cc=$1
  shift
  echo 'int main(void){return 0;}' | "$cc" "$@" -x c -o /dev/null - >/dev/null 2>&1
}

# Best target flags for this host (-march=native, or -mcpu=native on
# compilers/architectures that only know that). BENCH_ARCH_FLAGS overrides.
arch_flags() {
  if [[ -n "${BENCH_ARCH_FLAGS+x}" ]]; then echo "$BENCH_ARCH_FLAGS"; return; fi
  if _cc_accepts "$1" -march=native; then echo "-march=native"
  elif _cc_accepts "$1" -mcpu=native; then echo "-mcpu=native"
  fi
}

# build_tool NAME SOURCE [CFLAGS...] - compiles SOURCE for this host into
# $BUILD_DIR/NAME (rebuilt when sources, compiler or flags change) and
# prints the path of the binary.
build_tool() {
  local name=$1 src=$2 cc arch out sig rebuild=0 f
  shift 2
  cc=$(find_cc) || die "no C compiler found. Install one with: $(install_hint gcc)"
  mkdir -p "$BUILD_DIR" || die "cannot create $BUILD_DIR"
  out="$BUILD_DIR/$name"
  arch=$(arch_flags "$cc")
  # shellcheck disable=SC2206 # arch is intentionally word-split (may be empty)
  local flags=(-O3 $arch -std=gnu11 -pthread -Wall -Wextra -Wno-unused-parameter
               "-I$BENCH_ROOT/lib/c" "$@")
  sig="$cc ${flags[*]} | $("$cc" --version 2>/dev/null | head -1)"
  [[ -x $out && -f $out.flags && "$(cat "$out.flags")" == "$sig" ]] || rebuild=1
  for f in "$src" "$BENCH_ROOT"/lib/c/*.h; do
    [[ $f -nt $out ]] && rebuild=1
  done
  if (( rebuild )); then
    step "building $name with $cc ${arch:-(generic target)}" >&2
    if ! "$cc" "${flags[@]}" -o "$out.tmp" "$src" -lm > "$out.log" 2>&1; then
      cat "$out.log" >&2
      die "failed to build $name"
    fi
    mv -f "$out.tmp" "$out"
    printf '%s\n' "$sig" > "$out.flags"
  fi
  printf '%s\n' "$out"
}

# ---------------------------------------------------------------- helpers

# better A B [THRESHOLD] - true if A > B * (1 + THRESHOLD) (default 3%)
better() { awk -v a="$1" -v b="$2" -v t="${3:-0.03}" 'BEGIN { exit !(a + 0 > (b + 0) * (1 + t)) }'; }

# fdiv A B [DIGITS] - A / B with fixed decimals
fdiv() { awk -v a="$1" -v b="$2" -v d="${3:-2}" 'BEGIN { if (b + 0 == 0) print "0"; else printf "%.*f\n", d, a / b }'; }

# fmul A B [DIGITS]
fmul() { awk -v a="$1" -v b="$2" -v d="${3:-2}" 'BEGIN { printf "%.*f\n", d, a * b }'; }

# kv_get LINE KEY - value of KEY=value in a "RESULT k=v ..." line
kv_get() {
  local w words
  read -ra words <<< "$1"
  for w in ${words[@]+"${words[@]}"}; do
    if [[ $w == "$2="* ]]; then printf '%s\n' "${w#*=}"; return 0; fi
  done
  return 1
}

# human_bytes N - binary units
human_bytes() {
  awk -v b="$1" 'BEGIN { split("B KiB MiB GiB TiB PiB", u, " "); i = 1
    while (b >= 1024 && i < 6) { b /= 1024; i++ }
    printf (b == int(b) ? "%d %s\n" : "%.1f %s\n"), b, u[i] }'
}

# parse_size 4G|512M|100K|123 - bytes (binary multiples)
parse_size() {
  awk -v s="$1" 'BEGIN {
    if (s !~ /^[0-9]+(\.[0-9]+)?[KkMmGgTt]?([iI]?[bB])?$/) exit 1
    t = s; sub(/[iI]?[bB]$/, "", t); u = toupper(substr(t, length(t)))
    m = (u == "K") ? 1024 : (u == "M") ? 1048576 : (u == "G") ? 1073741824 : (u == "T") ? 1099511627776 : 1
    printf "%.0f\n", (t + 0) * m }'
}

# expand_cpulist "0-3,8" - one CPU id per line
expand_cpulist() {
  awk -v s="$1" 'BEGIN { n = split(s, p, ",")
    for (i = 1; i <= n; i++) {
      if (split(p[i], r, "-") == 2) { for (c = r[1] + 0; c <= r[2] + 0; c++) print c }
      else if (p[i] != "") print p[i] + 0 } }'
}

# count_list "1,2,3" - number of comma separated items
count_list() { local a; IFS=, read -ra a <<< "$1"; echo "${#a[@]}"; }

# tcp_probe HOST PORT - true if a TCP connection can be opened (gives up
# after 2 s, so hosts that silently drop packets do not hang the caller)
tcp_probe() {
  if have timeout; then
    timeout 2 bash -c 'exec 3<>"/dev/tcp/$1/$2"' _ "$1" "$2" 2>/dev/null
  else
    (exec 3<>"/dev/tcp/$1/$2") 2>/dev/null
  fi
}

# wait_port HOST PORT [SECONDS] - waits until a TCP connect succeeds
wait_port() {
  local end=$(( $(date +%s) + ${3:-10} ))
  while (( $(date +%s) < end )); do
    tcp_probe "$1" "$2" && return 0
    sleep 0.1
  done
  return 1
}

# port_in_use PORT - true if something listens on localhost:PORT
port_in_use() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

# raise_nofile - raises the soft open-files limit to the hard limit, prints it
raise_nofile() {
  local hard
  hard=$(ulimit -Hn)
  [[ $hard == unlimited ]] && hard=1048576
  ulimit -Sn "$hard" 2>/dev/null || true
  ulimit -Sn
}

# suite_begin NAME TITLE - common start of every suite script
suite_begin() {
  results_init
  [[ -n "$(sysinfo_get hostname)" ]] || sysinfo_collect >/dev/null
  header "$2"
  step "results: $BENCH_RESULTS_DIR (profile: $BENCH_PROFILE)"
}

# suite_end NAME - prints the suite's results and refreshes the reports
suite_end() {
  info "$1 results"
  print_results "$1"
  results_finalize
}
