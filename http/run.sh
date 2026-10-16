#!/usr/bin/env bash
# http/run.sh - HTTP request rate with a shared-nothing epoll server (httpd)
# and a closed-loop load generator (httpload). Auto-tuned: on one machine
# the CPU split between server and load generator and the number of
# connections are searched for the highest request rate.
#
# Author: Oleg Zhurko <oleg@zhurko.de>
# SPDX-License-Identifier: MIT
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
. "$HERE/../lib/common.sh"
# shellcheck source=../lib/hw.sh
. "$HERE/../lib/hw.sh"

ALL_SCENARIOS="plain,pipelined,json,compute"

usage() {
  cat <<EOF
Usage: http/run.sh [local] [options]       server and load generator on this machine
       http/run.sh server [options]        only the server, for a remote load generator
       http/run.sh client URL [options]    load generator against http://host:port

Scenarios:
  plain      GET /plain -> "Hello, World!" (13 bytes)
  pipelined  the same with N requests in flight per connection (HTTP pipelining)
  json       GET /json?items=N -> N mock user records serialized per request
  compute    GET /compute?rounds=N -> N chained SHA-256 rounds per request

Local mode gives the server and the load generator separate physical cores
(SMT siblings stay together), tries several splits and doubles the number of
connections until the request rate stops improving by >3%.

Options:
  -s, --scenarios LIST     $ALL_SCENARIOS (default: all)
      --json-items N       records per /json response (default 20, ~3.9 KB)
      --compute-rounds N   SHA-256 rounds per /compute request (default 1000)
      --pipeline N         depth of the pipelined scenario (default 16)
  -c, --connections LIST   connection counts to try (default: doubling from 2x threads)
      --split LIST         server share of the cores to try, percent (default 50,67,75,88)
  -p, --port PORT          server port (default 8080; local mode picks a free one)
  -b, --bind ADDR          server mode bind address (default 0.0.0.0)
      --threads N          server threads (server mode) or load generator threads (client mode)
      --loadgen NAME       httpload (default) or wrk
      --probe SEC          duration of each search probe (default 4, quick 2, long 8)
  -d, --duration SEC       final measurement (default 10, quick 5, long 30)
$COMMON_OPTIONS_HELP
EOF
}

mode=local URL=""
case ${1:-} in
  local) shift ;;
  server) mode=server; shift ;;
  client) mode=client; shift; [[ $# -ge 1 && $1 == http://* ]] || die "client mode needs a URL like http://host:8080"; URL=${1%/}; shift ;;
esac
opt_scen=$ALL_SCENARIOS ITEMS=20 ROUNDS=1000 PIPELINE=16 opt_conns="" opt_split="50,67,75,88"
PORT="" BIND=0.0.0.0 opt_threads="" LOADGEN=httpload opt_probe="" opt_dur=""
while (( $# )); do
  case $1 in
    -s|--scenarios) req_arg "$1" $#; opt_scen=$2; shift 2 ;;
    --json-items) req_arg "$1" $#; ITEMS=$2; shift 2 ;;
    --compute-rounds) req_arg "$1" $#; ROUNDS=$2; shift 2 ;;
    --pipeline) req_arg "$1" $#; PIPELINE=$2; shift 2 ;;
    -c|--connections) req_arg "$1" $#; opt_conns=$2; shift 2 ;;
    --split) req_arg "$1" $#; opt_split=$2; shift 2 ;;
    -p|--port) req_arg "$1" $#; PORT=$2; shift 2 ;;
    -b|--bind) req_arg "$1" $#; BIND=$2; shift 2 ;;
    --threads) req_arg "$1" $#; opt_threads=$2; shift 2 ;;
    --loadgen) req_arg "$1" $#; LOADGEN=$2; shift 2 ;;
    --probe) req_arg "$1" $#; opt_probe=$2; shift 2 ;;
    -d|--duration) req_arg "$1" $#; opt_dur=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) n=0; common_opt "$@" || n=$?; (( n )) || die "unknown option: $1 (see --help)"; shift "$n" ;;
  esac
done
for s in ${opt_scen//,/ }; do
  [[ ",$ALL_SCENARIOS," == *",$s,"* ]] || die "unknown scenario '$s' (available: $ALL_SCENARIOS)"
done
[[ $LOADGEN == httpload || $LOADGEN == wrk ]] || die "--loadgen must be httpload or wrk"

NOFILE=$(raise_nofile)

# ------------------------------------------------------------- server mode
if [[ $mode == server ]]; then
  HTTPD=$(build_tool httpd "$HERE/httpd.c") || exit 1
  threads=${opt_threads:-$(hw_ncpus)}
  PORT=${PORT:-8080}
  (( NOFILE < 16384 )) && warn "open files limit is $NOFILE; raise it ('ulimit -n') for many connections"
  info "httpd on $BIND:$PORT with $threads pinned worker threads (Ctrl+C to stop)"
  info "on the load generator host run:  http/run.sh client http://<this-host>:$PORT"
  exec "$HTTPD" -p "$PORT" -b "$BIND" -t "$threads" --pin
fi

# ------------------------------------------------------- load generation
suite_begin http "HTTP"
[[ $LOADGEN == wrk ]] && { have wrk || skip "wrk is not installed - install it with: $(install_hint wrk)"; }
HTTPD=$(build_tool httpd "$HERE/httpd.c") || exit 1
HTTPLOAD=$(build_tool httpload "$HERE/httpload.c") || exit 1
PROBE_T=${opt_probe:-$(ptime 4 2 8)}
FINAL_T=${opt_dur:-$(ptime 10 5 30)}

MAXCONN=$(( NOFILE - 256 ))
(( MAXCONN > 16384 )) && MAXCONN=16384
if [[ -r /proc/sys/net/ipv4/ip_local_port_range ]]; then # one ephemeral port per connection
  read -r plo phi < /proc/sys/net/ipv4/ip_local_port_range
  (( MAXCONN > (phi - plo) * 9 / 10 )) && MAXCONN=$(( (phi - plo) * 9 / 10 ))
fi
(( MAXCONN >= 16 )) || die "open files limit ($NOFILE) is too low; raise 'ulimit -n'"
HAVE_TASKSET=1
if ! have taskset; then
  HAVE_TASKSET=0
  warn "taskset not found: server and load generator are not pinned"
fi

pin() { # pin CPULIST CMD... - runs CMD restricted to CPULIST
  local cpus=$1
  shift
  if (( HAVE_TASKSET )); then taskset -c "$cpus" "$@"; else "$@"; fi
}

fmt_rps() { awk -v v="$1" 'BEGIN { s = sprintf("%.0f", v); r = ""
  while (length(s) > 3) { r = "," substr(s, length(s) - 2) r; s = substr(s, 1, length(s) - 3) }
  print s r }'; }
# latency in microseconds -> "850us" / "1.25ms" ("n/a" when unknown, e.g. wrk with pipelining)
fmt_us() { awk -v v="$1" 'BEGIN { if (v + 0 <= 0) print "n/a"; else if (v >= 1000) printf "%.2fms\n", v / 1000; else printf "%.0fus\n", v }'; }

# load URL CONNS PIPELINE CPULIST DURATION WARMUP - prints a RESULT line
load() {
  local url=$1 conns=$2 pipe=$3 cpus=$4 dur=$5 warm=$6 n
  n=$(count_list "$cpus")
  (( n > conns )) && n=$conns
  if [[ $LOADGEN == wrk ]]; then
    wrk_load "$url" "$conns" "$pipe" "$cpus" "$dur" "$warm" "$n"
    return
  fi
  pin "$cpus" "$HTTPLOAD" -c "$conns" -t "$n" --pin -d "$dur" -w "$warm" -p "$pipe" -q "$url" \
    2>> "$RAW_DIR/httpload.err" | tee -a "$RAW_DIR/httpload.log"
}

wrk_load() {
  local url=$1 conns=$2 pipe=$3 cpus=$4 dur=$5 warm=$6 n=$7 out script=()
  if (( pipe > 1 )); then
    cat > "$RAW_DIR/wrk-pipeline.lua" <<EOF
init = function(args)
  local r = {}
  for i = 1, $pipe do r[i] = wrk.format(nil) end
  req = table.concat(r)
end
request = function() return req end
EOF
    script=(-s "$RAW_DIR/wrk-pipeline.lua")
  fi
  if (( warm > 0 )); then
    pin "$cpus" wrk -t "$n" -c "$conns" -d "${warm}s" ${script[@]+"${script[@]}"} "$url" > /dev/null 2>&1
  fi
  out=$(pin "$cpus" wrk -t "$n" -c "$conns" -d "${dur}s" --latency ${script[@]+"${script[@]}"} "$url" 2>&1) || return 1
  printf '%s\n' "$out" >> "$RAW_DIR/wrk.log"
  awk -v c="$conns" -v p="$pipe" -v t="$n" '
    function us(v) { if (v ~ /us$/) return v + 0; if (v ~ /ms$/) return v * 1000; if (v ~ /s$/) return v * 1e6; return v + 0 }
    /Requests\/sec:/ { rps = $2 }
    /^ +Latency / { avg = us($2); mx = us($4) }
    /^ +50%/ { p50 = us($2) }
    /^ +90%/ { p90 = us($2) }
    /^ +99%/ { p99 = us($2) }
    / requests in / { req = $1 }
    /Socket errors:/ { gsub(/,/, ""); err = $4 + $6 + $8 + $10 }
    /Non-2xx/ { non = $NF }
    /Transfer\/sec:/ { v = $2; mb = (v ~ /GB/) ? v * 1073.74 : (v ~ /MB/) ? v * 1.04858 : (v ~ /KB/) ? v * 0.001024 : v / 1e6 }
    END { printf "RESULT rps=%s requests=%d mbps=%.3f lat_avg_us=%.2f lat_p50_us=%.2f lat_p90_us=%.2f lat_p99_us=%.2f lat_max_us=%.2f non2xx=%d err_connect=0 err_read=%d err_timeout=0 err_parse=0 threads=%d connections=%d pipeline=%d\n",
              rps, req, mb, avg, p50, p90, p99, mx, non, err, t, c, p }' <<< "$out"
}

# valid LINE - at least one response and less than 1% errors
valid() {
  local req err=0 k v
  req=$(kv_get "$1" requests) || return 1
  for k in non2xx err_connect err_read err_timeout err_parse; do
    v=$(kv_get "$1" "$k") && err=$(( err + v ))
  done
  (( req > 0 && err * 100 <= req ))
}

# sweep_conns URL PIPELINE CLIENT_CPUS START - sets BEST_C BEST_LINE
sweep_conns() {
  local url=$1 pipe=$2 ccpus=$3 c=$4 list="" best=0 line rps
  if [[ -n $opt_conns ]]; then
    list=${opt_conns//,/ }
  else
    (( c < 8 )) && c=8
    while (( c <= MAXCONN )); do list+="$c "; c=$(( c * 2 )); done
  fi
  BEST_C=0 BEST_LINE=""
  for c in $list; do
    line=$(load "$url" "$c" "$pipe" "$ccpus" "$PROBE_T" 1)
    if ! valid "$line"; then
      step "conns=$c: failed or more than 1% errors ($(kv_get "$line" requests || echo 0) requests)"
      break
    fi
    rps=$(kv_get "$line" rps)
    step "$(printf 'conns=%-6s %12s req/s   p50 %-8s p99 %s' "$c" "$(fmt_rps "$rps")" \
      "$(fmt_us "$(kv_get "$line" lat_p50_us)")" "$(fmt_us "$(kv_get "$line" lat_p99_us)")")"
    if better "$rps" "$best"; then best=$rps BEST_C=$c BEST_LINE=$line; else break; fi
  done
}

record() { # record NAME LINE EXTRA_DETAILS
  local line=$2 rps
  rps=$(kv_get "$line" rps)
  good "$1: $(fmt_rps "$rps") req/s, p50 $(fmt_us "$(kv_get "$line" lat_p50_us)"), p99 $(fmt_us "$(kv_get "$line" lat_p99_us)"), $(kv_get "$line" mbps) MB/s"
  result http "$1" "$(fdiv "$rps" 1 0)" "req/s" \
    "conns=$(kv_get "$line" connections) pipeline=$(kv_get "$line" pipeline) $3 p50=$(fmt_us "$(kv_get "$line" lat_p50_us)") p99=$(fmt_us "$(kv_get "$line" lat_p99_us)") MB/s=$(kv_get "$line" mbps)"
}

# --------------------------------------------------------- remote client
if [[ $mode == client ]]; then
  hostport=${URL#http://}
  hostport=${hostport%%/*}
  if [[ $hostport == \[*\]* ]]; then # [IPv6]:port
    host=${hostport#\[} host=${host%%\]*} rport=${hostport##*\]}
    rport=${rport#:}
  else
    host=${hostport%%:*} rport=${hostport#"$host"}
    rport=${rport#:}
  fi
  rport=${rport:-80}
  wait_port "$host" "$rport" 5 || die "cannot connect to $host:$rport (start 'http/run.sh server' there)"
  mapfile -t order < <(hw_core_groups | tr ',' '\n')
  if [[ -n $opt_threads ]]; then order=("${order[@]:0:$opt_threads}"); fi
  CCPUS=$(IFS=,; echo "${order[*]}")
  cn=$(count_list "$CCPUS")
  sysinfo_set http_target "$URL"
  step "target $URL, load generator on $cn CPUs, probes ${PROBE_T}s, finals ${FINAL_T}s"
  for sc in ${opt_scen//,/ }; do
    case $sc in
      plain) name=plain path=/plain pipe=1 ;;
      pipelined) name=plain_pipelined path=/plain pipe=$PIPELINE ;;
      json) name=json path="/json?items=$ITEMS" pipe=1 ;;
      compute) name=compute path="/compute?rounds=$ROUNDS" pipe=1 ;;
    esac
    info "$name: GET $path$( (( pipe > 1 )) && echo " (pipeline $pipe)")"
    sweep_conns "$URL$path" "$pipe" "$CCPUS" $(( 2 * cn ))
    (( BEST_C > 0 )) || { warn "$name: no valid measurement"; continue; }
    line=$(load "$URL$path" "$BEST_C" "$pipe" "$CCPUS" "$FINAL_T" 2)
    valid "$line" || { warn "$name: final run failed"; continue; }
    record "$name" "$line" "server=remote client_cpus=$cn"
  done
  suite_end http
  exit 0
fi

# ------------------------------------------------------------- local mode
mapfile -t CORES < <(hw_core_groups)
G=${#CORES[@]}
cpus_of() { # cpus_of FROM TO - CPU list of cores [FROM, TO)
  local i out=""
  for (( i = $1; i < $2; i++ )); do out+="${out:+,}${CORES[i]}"; done
  echo "$out"
}
splits=()
if (( G >= 2 )); then
  for pct in ${opt_split//,/ }; do
    s=$(( (G * pct + 50) / 100 ))
    (( s < 1 )) && s=1
    (( s > G - 1 )) && s=$(( G - 1 ))
    [[ " ${splits[*]-} " == *" $s "* ]] || splits+=("$s")
  done
fi
server_cpus() { if (( G >= 2 )); then cpus_of 0 "$1"; else cpus_of 0 "$G"; fi; }
client_cpus() { if (( G >= 2 )); then cpus_of "$1" "$G"; else cpus_of 0 "$G"; fi; }
(( G >= 2 )) || { splits=(0); warn "only one core: server and load generator share it"; }

if [[ -z $PORT ]]; then
  PORT=$(( 18080 + RANDOM % 2000 ))
  while port_in_use "$PORT"; do PORT=$(( PORT + 1 )); done
fi
SERVER_PID=""
server_stop() {
  if [[ -n $SERVER_PID ]]; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
  SERVER_PID=""
}
server_start() { # server_start CPULIST
  local args=(-p "$PORT" -b 127.0.0.1 -t "$(count_list "$1")" --pin -q)
  server_stop
  # started directly (taskset execs httpd) so that $! is the server itself
  if (( HAVE_TASKSET )); then taskset -c "$1" "$HTTPD" "${args[@]}" &
  else "$HTTPD" "${args[@]}" &
  fi
  SERVER_PID=$!
  wait_port 127.0.0.1 "$PORT" 10 || die "httpd did not start on port $PORT"
}
on_exit server_stop

step "$G cores; server/load generator core splits to try: $(for s in "${splits[@]}"; do printf '%s/%s ' "$s" $(( G - s )); done)"
step "port $PORT, open files limit $NOFILE, max connections $MAXCONN, probes ${PROBE_T}s, finals ${FINAL_T}s, load generator $LOADGEN"

run_scenario() { # run_scenario NAME PATH PIPELINE
  local name=$1 path=$2 pipe=$3 url="http://127.0.0.1:$PORT$2" s si sc cc best_s best_c best_rps line rps
  info "$name: GET $path$( (( pipe > 1 )) && echo " (pipeline $pipe)")"
  # 1) connection sweep at the first split
  s=${splits[0]}
  sc=$(server_cpus "$s") cc=$(client_cpus "$s")
  step "server on CPUs [$sc], load generator on CPUs [$cc]"
  server_start "$sc"
  sweep_conns "$url" "$pipe" "$cc" $(( 2 * $(count_list "$sc") + 2 * $(count_list "$cc") ))
  (( BEST_C > 0 )) || { warn "$name: no valid measurement"; return 1; }
  best_s=$s best_c=$BEST_C best_rps=$(kv_get "$BEST_LINE" rps)
  # 2) other splits at that connection count
  for (( si = 1; si < ${#splits[@]}; si++ )); do
    s=${splits[si]}
    sc=$(server_cpus "$s") cc=$(client_cpus "$s")
    server_start "$sc"
    line=$(load "$url" "$best_c" "$pipe" "$cc" "$PROBE_T" 1)
    valid "$line" || continue
    rps=$(kv_get "$line" rps)
    step "$(printf 'split %s/%s cores, conns=%-6s %12s req/s' "$s" $(( G - s )) "$best_c" "$(fmt_rps "$rps")")"
    if better "$rps" "$best_rps" 0.01; then best_rps=$rps best_s=$s; fi
  done
  sc=$(server_cpus "$best_s") cc=$(client_cpus "$best_s")
  # 3) a different split may want more connections
  if [[ $best_s != "${splits[0]}" && -z $opt_conns ]] && (( best_c * 2 <= MAXCONN )); then
    server_start "$sc"
    line=$(load "$url" $(( best_c * 2 )) "$pipe" "$cc" "$PROBE_T" 1)
    if valid "$line" && better "$(kv_get "$line" rps)" "$best_rps"; then
      best_c=$(( best_c * 2 ))
      step "$(printf 'split %s/%s cores, conns=%-6s %12s req/s' "$best_s" $(( G - best_s )) "$best_c" "$(fmt_rps "$(kv_get "$line" rps)")")"
    fi
  fi
  # 4) final measurement
  step "final: server CPUs [$sc], load generator CPUs [$cc], conns=$best_c, ${FINAL_T}s"
  server_start "$sc"
  line=$(load "$url" "$best_c" "$pipe" "$cc" "$FINAL_T" 2)
  server_stop
  valid "$line" || { warn "$name: final run failed or had errors"; return 1; }
  record "$name" "$line" "server_cpus=$(count_list "$sc") client_cpus=$(count_list "$cc")"
}

for sc in ${opt_scen//,/ }; do
  case $sc in
    plain) run_scenario plain /plain 1 ;;
    pipelined) run_scenario plain_pipelined /plain "$PIPELINE" ;;
    json) run_scenario json "/json?items=$ITEMS" 1 ;;
    compute) run_scenario compute "/compute?rounds=$ROUNDS" 1 ;;
  esac
done

suite_end http
