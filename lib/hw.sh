#!/usr/bin/env bash
# shellcheck shell=bash
#
# lib/hw.sh - hardware and OS detection used to size and tune the
# benchmarks, plus the system information report.
#
# Author: Oleg Zhurko <oleg@zhurko.de>
# SPDX-License-Identifier: MIT

[[ -n "${_BENCH_HW_SH:-}" ]] && return 0
_BENCH_HW_SH=1

# CPU ids this process may run on (affinity / cpuset), one per line
hw_cpus_allowed() {
  local list n
  list=$(awk '/^Cpus_allowed_list:/ { print $2 }' /proc/self/status 2>/dev/null)
  if [[ -n $list ]]; then
    expand_cpulist "$list"
  else
    n=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
    seq 0 $(( n - 1 ))
  fi
}

hw_ncpus() { hw_cpus_allowed | wc -l | tr -d ' '; }

# CPU limit of the cgroup (containers), e.g. "2.00"; empty when unlimited
hw_cpu_quota() {
  local cg q p f
  cg=$(awk -F: '$1 == "0" { print $3; exit }' /proc/self/cgroup 2>/dev/null)
  for f in "/sys/fs/cgroup$cg/cpu.max" /sys/fs/cgroup/cpu.max; do
    if [[ -r $f ]]; then
      read -r q p < "$f"
      [[ $q == max ]] && return 0
      awk -v q="$q" -v p="$p" 'BEGIN { printf "%.2f\n", q / p }'
      return 0
    fi
  done
  if [[ -r /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]]; then
    read -r q < /sys/fs/cgroup/cpu/cpu.cfs_quota_us
    read -r p < /sys/fs/cgroup/cpu/cpu.cfs_period_us
    (( q > 0 )) && awk -v q="$q" -v p="$p" 'BEGIN { printf "%.2f\n", q / p }'
  fi
  return 0
}

# One line per physical core among the allowed CPUs: the comma separated
# CPU ids of its hardware threads, ordered by socket, then core.
hw_core_groups() {
  local cpu sib pkg base=/sys/devices/system/cpu
  for cpu in $(hw_cpus_allowed); do
    sib=$cpu pkg=0
    [[ -r $base/cpu$cpu/topology/thread_siblings_list ]] &&
      read -r sib < "$base/cpu$cpu/topology/thread_siblings_list"
    [[ -r $base/cpu$cpu/topology/physical_package_id ]] &&
      read -r pkg < "$base/cpu$cpu/topology/physical_package_id"
    printf '%s %s %s\n' "$pkg" "$sib" "$cpu"
  done | awk '{
      key = $1 ":" $2
      if (!(key in g)) { n++; keys[n] = key; pkg[key] = $1; first[key] = $3; g[key] = $3 }
      else g[key] = g[key] "," $3
    }
    END { for (i = 1; i <= n; i++) print pkg[keys[i]], first[keys[i]], g[keys[i]] }' |
    sort -k1,1n -k2,2n | awk '{ print $3 }'
}

hw_ncores() { hw_core_groups | wc -l | tr -d ' '; }

hw_sockets() {
  local n
  n=$(cat /sys/devices/system/cpu/cpu[0-9]*/topology/physical_package_id 2>/dev/null | sort -u | wc -l)
  echo $(( n > 0 ? n : 1 ))
}

hw_numa_nodes() {
  local n
  n=$(find /sys/devices/system/node -maxdepth 1 -name 'node[0-9]*' 2>/dev/null | wc -l)
  echo $(( n > 0 ? n : 1 ))
}

hw_cpu_model() {
  local m
  m=$(LC_ALL=C lscpu 2>/dev/null | awk -F: '/^Model name/ { sub(/^[ \t]+/, "", $2); print $2; exit }')
  [[ -z $m ]] && m=$(awk -F: '/^(model name|Hardware|cpu model|uarch)/ { sub(/^[ \t]+/, "", $2); print $2; exit }' /proc/cpuinfo 2>/dev/null)
  echo "${m:-unknown}"
}

hw_cpu_flags() { awk -F: '/^(flags|Features)[ \t]*:/ { print $2; exit }' /proc/cpuinfo 2>/dev/null; }

hw_simd() {
  local f
  f=" $(hw_cpu_flags) "
  case $(uname -m) in
    x86_64|i?86)
      if [[ $f == *" avx512f "* ]]; then echo AVX-512
      elif [[ $f == *" avx2 "* ]]; then echo AVX2
      elif [[ $f == *" avx "* ]]; then echo AVX
      else echo SSE; fi ;;
    aarch64|arm64)
      if [[ $f == *" sve2 "* ]]; then echo "NEON+SVE2"
      elif [[ $f == *" sve "* ]]; then echo "NEON+SVE"
      else echo NEON; fi ;;
    *) uname -m ;;
  esac
}

hw_cpu_max_mhz() {
  local f=/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq v
  if [[ -r $f ]]; then read -r v < "$f"; echo $(( v / 1000 )); fi
}

hw_governor() { cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null; }

# "on" / "off" / "" (unknown) - CPU turbo / boost state
hw_turbo() {
  local v
  if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    read -r v < /sys/devices/system/cpu/intel_pstate/no_turbo
    [[ $v == 0 ]] && echo on || echo off
  elif [[ -r /sys/devices/system/cpu/cpufreq/boost ]]; then
    read -r v < /sys/devices/system/cpu/cpufreq/boost
    [[ $v == 1 ]] && echo on || echo off
  fi
}

hw_smt() { cat /sys/devices/system/cpu/smt/control 2>/dev/null; }

# cache hierarchy seen by CPU 0, e.g. "L1d 48K, L1i 32K, L2 2048K, L3 260M"
hw_caches() {
  local d lvl type size out=""
  for d in /sys/devices/system/cpu/cpu0/cache/index[0-9]*; do
    [[ -r $d/level && -r $d/size ]] || continue
    read -r lvl < "$d/level"
    read -r type < "$d/type"
    read -r size < "$d/size"
    case $type in Data) type=d ;; Instruction) type=i ;; *) type= ;; esac
    out+="${out:+, }L$lvl$type $size"
  done
  echo "$out"
}

hw_mem_total() { awk '/^MemTotal:/ { printf "%.0f\n", $2 * 1024; exit }' /proc/meminfo; }
hw_mem_avail() { awk '/^MemAvailable:/ { printf "%.0f\n", $2 * 1024; exit }' /proc/meminfo; }

# DIMM population, e.g. "8x 32 GB DDR5 @ 4800 MT/s" (needs root and dmidecode)
hw_dimms() {
  (( EUID == 0 )) && have dmidecode || return 0
  dmidecode -t 17 2>/dev/null | awk -F': ' '
    function flush() {
      if (size != "" && size !~ /No Module|Not Installed|^0/) c[size " " type " @ " speed]++
      size = type = speed = ""
    }
    /^Memory Device/ { flush() }
    /^\tSize:/ { size = $2 }
    /^\tType:/ { type = $2 }
    /^\tSpeed:/ { if (speed == "" && $2 !~ /Unknown/) speed = $2 }
    /^\tConfigured (Memory|Clock) Speed:/ { if ($2 !~ /Unknown/) speed = $2 }
    END { flush(); for (k in c) printf "%s%dx %s", (n++ ? ", " : ""), c[k], k; if (n) print "" }'
}

hw_os() {
  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    ( . /etc/os-release; echo "${PRETTY_NAME:-${NAME:-Linux}}" )
  else
    uname -s
  fi
}

hw_virt() {
  local v=""
  have systemd-detect-virt && v=$(systemd-detect-virt 2>/dev/null)
  if [[ -z $v ]]; then
    if grep -qw hypervisor /proc/cpuinfo 2>/dev/null; then v="vm"; else v="none"; fi
  fi
  echo "$v"
}

hw_thp() {
  local s
  s=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null) || return 0
  s=${s#*[}
  echo "${s%%]*}"
}

# block devices: "name size rotational transport model"
hw_disks() {
  lsblk -d -n -o NAME,SIZE,ROTA,TRAN,MODEL 2>/dev/null |
    awk '$1 !~ /^(loop|ram|zram|sr|fd|nbd)/ { $1 = $1; print }'
}

# physical network interfaces: "name speed_mbit driver"
hw_nics() {
  local d n speed drv
  for d in /sys/class/net/*; do
    n=${d##*/}
    [[ $n == lo || ! -e $d/device ]] && continue
    speed=$(cat "$d/speed" 2>/dev/null) || speed=""
    [[ $speed =~ ^[0-9]+$ ]] || speed="?"
    drv=$(readlink "$d/device/driver" 2>/dev/null)
    printf '%s %s %s\n' "$n" "$speed" "${drv##*/}"
  done
}

# link speed of an interface in Mbit/s (empty if unknown)
hw_link_speed() {
  local s
  s=$(cat "/sys/class/net/$1/speed" 2>/dev/null) || return 0
  [[ $s =~ ^[0-9]+$ ]] && (( s > 0 )) && echo "$s"
  return 0
}

# Collects the system description into sysinfo.tsv and prints it.
sysinfo_collect() {
  local cpus cores quota
  cpus=$(hw_ncpus)
  cores=$(hw_ncores)
  quota=$(hw_cpu_quota)
  sysinfo_set hostname "$(short_hostname)"
  sysinfo_set date "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  sysinfo_set os "$(hw_os)"
  sysinfo_set kernel "$(uname -r)"
  sysinfo_set arch "$(uname -m)"
  sysinfo_set virtualization "$(hw_virt)"
  sysinfo_set cpu_model "$(hw_cpu_model)"
  sysinfo_set cpus "$cpus"
  sysinfo_set cores "$cores"
  sysinfo_set sockets "$(hw_sockets)"
  sysinfo_set numa_nodes "$(hw_numa_nodes)"
  sysinfo_set simd "$(hw_simd)"
  sysinfo_set caches "$(hw_caches)"
  sysinfo_set cpu_max_mhz "$(hw_cpu_max_mhz)"
  sysinfo_set governor "$(hw_governor)"
  sysinfo_set turbo "$(hw_turbo)"
  sysinfo_set smt "$(hw_smt)"
  sysinfo_set cpu_quota "$quota"
  sysinfo_set mem_total_bytes "$(hw_mem_total)"
  sysinfo_set mem_total "$(human_bytes "$(hw_mem_total)")"
  sysinfo_set dimms "$(hw_dimms)"
  sysinfo_set thp "$(hw_thp)"
  sysinfo_set disks "$(hw_disks | paste -sd ';' - | sed 's/;/; /g')"
  sysinfo_set nics "$(hw_nics | paste -sd ';' - | sed 's/;/; /g')"
  sysinfo_set compiler "$(cc_version)"
  sysinfo_set suite_version "$BENCH_VERSION"
  awk -F'\t' '$2 != "" { printf "  %-16s %s\n", $1, $2 }' "$SYSINFO_TSV"
}

# Prints hints about settings that keep a machine below its maximum.
sysinfo_warnings() {
  local gov virt quota thp
  gov=$(sysinfo_get governor)
  virt=$(sysinfo_get virtualization)
  quota=$(sysinfo_get cpu_quota)
  thp=$(sysinfo_get thp)
  if [[ -n $gov && $gov != performance ]]; then
    warn "CPU frequency governor is '$gov'. For peak numbers use './bench.sh --tune' (root) or 'cpupower frequency-set -g performance'."
  fi
  if [[ $(sysinfo_get turbo) == off ]]; then
    warn "CPU turbo/boost is disabled; single-thread results will be below the CPU's maximum."
  fi
  if [[ -n $quota ]]; then
    warn "a cgroup CPU quota of $quota CPUs is active (container limit); multi-thread results are capped by it."
  fi
  if [[ -n $virt && $virt != none ]]; then
    warn "running under virtualization ($virt): results describe this VM/container, not the host hardware."
  fi
  if [[ $thp == never ]]; then
    step "note: transparent huge pages are disabled; memory latency will include TLB misses."
  fi
}
