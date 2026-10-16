#!/usr/bin/env bash
# network/run.sh - TCP throughput with iperf3, auto-tuned: the number of
# parallel streams is swept (spread over several iperf3 processes, so old
# single-threaded iperf3 versions scale too), zero-copy is tried, then
# upload, download and bidirectional throughput are measured.
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
Usage: network/run.sh [loopback]            local TCP stack (no NIC involved)
       network/run.sh server [options]      iperf3 servers for a remote client
       network/run.sh client HOST [options] measure this host <-> HOST

Run 'server' on the remote machine first (it prints the client command),
then 'client HOST' here. Ports PORT..PORT+INSTANCES-1 must be reachable.

Options:
  -p, --port PORT        first iperf3 port (default 5201)
  -i, --instances N      iperf3 server processes (default: min(CPUs, 8))
  -P, --streams LIST     stream counts to try (default: 1,2,4,8,16,32,64 up to 4x CPUs)
  -t, --time SEC         final test duration (default 10, quick 5, long 30)
      --probe SEC        duration of each sweep probe (default 4, quick 2, long 8)
      --udp              also measure UDP throughput and loss (-b 0)
      --no-bidir         skip the bidirectional test
$COMMON_OPTIONS_HELP
EOF
}

mode=loopback HOST="" PORT=5201 opt_inst="" opt_streams="" opt_time="" opt_probe="" opt_udp=0 opt_bidir=1
case ${1:-} in
  loopback) shift ;;
  server) mode=server; shift ;;
  client) mode=client; shift; [[ $# -ge 1 && $1 != -* ]] || die "client mode needs the server host"; HOST=$1; shift ;;
esac
while (( $# )); do
  case $1 in
    -p|--port) req_arg "$1" $#; PORT=$2; shift 2 ;;
    -i|--instances) req_arg "$1" $#; opt_inst=$2; shift 2 ;;
    -P|--streams) req_arg "$1" $#; opt_streams=$2; shift 2 ;;
    -t|--time) req_arg "$1" $#; opt_time=$2; shift 2 ;;
    --probe) req_arg "$1" $#; opt_probe=$2; shift 2 ;;
    --udp) opt_udp=1; shift ;;
    --no-bidir) opt_bidir=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) n=0; common_opt "$@" || n=$?; (( n )) || die "unknown option: $1 (see --help)"; shift "$n" ;;
  esac
done

NCPU=$(hw_ncpus)
NINST=${opt_inst:-$(( NCPU < 8 ? NCPU : 8 ))}

start_servers() { # start_servers BIND_ADDR - NINST iperf3 servers on PORT..
  local i bind=() pids=()
  [[ -n $1 ]] && bind=(-B "$1")
  for (( i = 0; i < NINST; i++ )); do
    port_in_use $(( PORT + i )) && die "port $(( PORT + i )) is already in use (choose another --port)"
    iperf3 -s -p $(( PORT + i )) ${bind[@]+"${bind[@]}"} > "${RAW_DIR:-/tmp}/iperf3-server-$i.log" 2>&1 &
    pids+=($!)
  done
  # shellcheck disable=SC2034 # used by the exit hook below
  SERVER_PIDS=("${pids[@]}")
  on_exit 'kill "${SERVER_PIDS[@]}" 2>/dev/null; wait "${SERVER_PIDS[@]}" 2>/dev/null'
  for (( i = 0; i < NINST; i++ )); do
    wait_port "${1:-127.0.0.1}" $(( PORT + i )) 10 || die "iperf3 server on port $(( PORT + i )) did not start"
  done
}

# ------------------------------------------------------------- server mode
if [[ $mode == server ]]; then
  have iperf3 || die "iperf3 is not installed - install it with: $(install_hint iperf3)"
  RAW_DIR=$(mktemp -d)
  on_exit "rm -rf $(printf %q "$RAW_DIR")"
  start_servers ""
  ips=$( (hostname -I 2>/dev/null || ip -o -4 addr show scope global 2>/dev/null | awk '{ sub(/\/.*/, "", $4); print $4 }') | tr '\n' ' ')
  info "$NINST iperf3 servers listening on ports $PORT-$(( PORT + NINST - 1 ))"
  info "on the client run:  network/run.sh client ${ips%% *} --port $PORT --instances $NINST"
  [[ -n $ips ]] && step "addresses of this host: $ips"
  step "press Ctrl+C to stop"
  wait
  exit 0
fi

# ---------------------------------------------------------- client side
suite_begin network "Network"
have iperf3 || skip "iperf3 is not installed - install it with: $(install_hint iperf3)"

FINAL_T=${opt_time:-$(ptime 10 5 30)}
PROBE_T=${opt_probe:-$(ptime 4 2 8)}
OMIT=1
iver=$(iperf3 --version 2>&1 | awk 'NR == 1 { print $2 }')
step "iperf3 $iver; probes ${PROBE_T}s, finals ${FINAL_T}s (first ${OMIT}s omitted)"

if [[ $mode == loopback ]]; then
  HOST=127.0.0.1
  for (( try = 0; try < 20; try++ )); do # a free range of NINST ports
    PORT=$(( 15201 + RANDOM % 20000 )) busy=0
    for (( i = 0; i < NINST; i++ )); do port_in_use $(( PORT + i )) && busy=1; done
    (( busy )) || break
  done
  start_servers 127.0.0.1
  step "loopback: measures the kernel TCP/IP stack and memory bandwidth, not a NIC"
else
  # how many server instances answer?
  found=0
  for (( i = 0; i < ${opt_inst:-16}; i++ )); do
    if tcp_probe "$HOST" $(( PORT + i )); then found=$(( found + 1 )); else break; fi
  done
  (( found > 0 )) || die "no iperf3 server reachable at $HOST:$PORT (start 'network/run.sh server' there)"
  (( found < NINST )) && step "$found iperf3 server instance(s) reachable on $HOST"
  NINST=$found
  sleep 1 # let the servers forget the probe connections
fi
sysinfo_set net_peer "$mode $HOST"

# interface used to reach HOST and its link speed
dev=""
if have ip; then
  dev=$(ip -o route get "$HOST" 2>/dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
elif [[ $mode == loopback ]]; then
  dev=lo
fi
speed=$( [[ -n $dev ]] && hw_link_speed "$dev")
[[ -n $dev ]] && step "interface: $dev${speed:+, link ${speed} Mbit/s}"
[[ -n $dev ]] && sysinfo_set net_interface "$dev${speed:+ ${speed}Mbit/s}"

if [[ $mode == client ]] && have ping; then
  rtt=$(ping -c 10 -i 0.2 -q "$HOST" 2>/dev/null | awk -F'/' '/min\/avg\/max/ { print $5 }')
  if [[ -n $rtt ]]; then
    step "ping rtt avg $rtt ms"
    result network rtt "$rtt" ms "ping -c 10 $HOST"
  fi
fi

# iperf_run TAG STREAMS SECONDS [ARGS...] - runs STREAMS streams spread over
# the server instances; prints "fwd_bps rev_bps retransmits"
iperf_run() {
  local tag=$1 streams=$2 secs=$3 inst per extra i p f files=() pids=()
  shift 3
  inst=$(( streams < NINST ? streams : NINST ))
  per=$(( streams / inst )) extra=$(( streams % inst ))
  for (( i = 0; i < inst; i++ )); do
    p=$(( per + (i < extra ? 1 : 0) ))
    f="$RAW_DIR/iperf3-$tag-P$streams-$i.json"
    timeout $(( secs + 30 )) iperf3 -c "$HOST" -p $(( PORT + i )) -P "$p" -t "$secs" -O "$OMIT" -J ${1+"$@"} > "$f" 2>&1 &
    pids+=($!)
    files+=("$f")
  done
  for p in "${pids[@]}"; do wait "$p"; done
  awk '
    FNR == 1 { sec = ""; inend = 0 }
    /"error":/ { e = $0; sub(/^[^:]*:[ \t]*/, "", e); err = err e " " }
    /"sum_sent":[ \t]*\{/ { sec = "sent"; next }
    /"sum_received":[ \t]*\{/ { sec = "recv"; next }
    /"sum_sent_bidir_reverse":[ \t]*\{/ { sec = "rsent"; next }
    /"sum_received_bidir_reverse":[ \t]*\{/ { sec = "rrecv"; next }
    /"sum":[ \t]*\{/ { sec = (inend ? "udp" : ""); next }
    /^\t"end":/ { inend = 1 }
    sec != "" && /"bits_per_second":/ { v = $0; sub(/.*:[ \t]*/, "", v); sub(/,.*/, "", v); bps[sec] += v }
    sec == "sent" && /"retransmits":/ { v = $0; sub(/.*:[ \t]*/, "", v); sub(/,.*/, "", v); retr += v }
    sec == "udp" && /"lost_percent":/ { v = $0; sub(/.*:[ \t]*/, "", v); sub(/,.*/, "", v); lost += v; nl++ }
    /^\t\t}/ { sec = "" }
    END {
      if (err != "") { print "ERROR " err > "/dev/stderr" }
      fwd = bps["recv"] > 0 ? bps["recv"] : bps["udp"]
      printf "%.0f %.0f %d %.2f\n", fwd, bps["rrecv"], retr, nl ? lost / nl : 0
      exit (fwd > 0 ? 0 : 1)
    }' "${files[@]}"
}

gbit() { awk -v b="$1" 'BEGIN { printf "%.2f", b / 1e9 }'; }
pct_of_link() { [[ -n $speed ]] && awk -v b="$1" -v s="$speed" 'BEGIN { printf " (%.0f%% of link)", b / (s * 1e4) }'; }

# ------------------------------------------------------------ stream sweep
info "sweeping parallel streams (upload)"
if [[ -n $opt_streams ]]; then
  stream_list=${opt_streams//,/ }
else
  stream_list=""
  for s in 1 2 4 8 16 32 64; do (( s <= 4 * NCPU )) && stream_list+="$s "; done
fi
best=0 best_s=1
for s in $stream_list; do
  out=$(iperf_run probe "$s" "$PROBE_T") || { warn "iperf3 failed with $s streams"; break; }
  read -r v _ <<< "$out"
  step "$(printf 'streams=%-3s %8s Gbit/s' "$s" "$(gbit "$v")")"
  if better "$v" "$best"; then best=$v best_s=$s; else break; fi
done
(( ${best%.*} > 0 )) || die "iperf3 produced no throughput (see $RAW_DIR/iperf3-probe-*.json)"
ZC=()
if out=$(iperf_run probe-zerocopy "$best_s" "$PROBE_T" -Z); then
  read -r v _ <<< "$out"
  step "$(printf 'streams=%-3s %8s Gbit/s with zero-copy (-Z)' "$best_s" "$(gbit "$v")")"
  better "$v" "$best" && ZC=(-Z)
fi
cfg="streams=$best_s processes=$(( best_s < NINST ? best_s : NINST ))${ZC[0]:+ zerocopy}"
info "final runs: $cfg, ${FINAL_T}s"

label=$([[ $mode == loopback ]] && echo "loopback_tcp" || echo "tcp")
if out=$(iperf_run upload "$best_s" "$FINAL_T" ${ZC[@]+"${ZC[@]}"}); then
  read -r f _ retr _ <<< "$out"
  good "upload:   $(gbit "$f") Gbit/s$(pct_of_link "$f"), retransmits $retr"
  result network "${label}_upload" "$(gbit "$f")" "Gbit/s" "$cfg retransmits=$retr"
fi
if [[ $mode == client ]]; then
  if out=$(iperf_run download "$best_s" "$FINAL_T" -R ${ZC[@]+"${ZC[@]}"}); then
    read -r f _ retr _ <<< "$out"
    good "download: $(gbit "$f") Gbit/s$(pct_of_link "$f")"
    result network "${label}_download" "$(gbit "$f")" "Gbit/s" "$cfg"
  fi
fi
if (( opt_bidir )) && iperf3 --help 2>&1 | grep -q -- '--bidir'; then
  if out=$(iperf_run bidir "$best_s" "$FINAL_T" --bidir ${ZC[@]+"${ZC[@]}"}); then
    read -r f r retr _ <<< "$out"
    tot=$(awk -v a="$f" -v b="$r" 'BEGIN { print a + b }')
    good "bidir:    $(gbit "$tot") Gbit/s total ($(gbit "$f") up + $(gbit "$r") down)"
    result network "${label}_bidir" "$(gbit "$tot")" "Gbit/s" "$cfg up=$(gbit "$f") down=$(gbit "$r")"
  fi
fi
if (( opt_udp )); then
  if out=$(iperf_run udp "$best_s" "$FINAL_T" -u -b 0 -l 1400); then
    read -r f _ _ lost <<< "$out"
    good "udp:      $(gbit "$f") Gbit/s, loss $lost%"
    result network "${label/tcp/udp}_upload" "$(gbit "$f")" "Gbit/s" "$cfg datagram=1400 loss=${lost}%"
  fi
fi

suite_end network
