#!/usr/bin/env bash
# disk/run.sh - storage benchmark with fio, auto-tuned for the device:
# sequential read/write bandwidth and 4K random read/write IOPS (queue depth
# and job count are hill-climbed until the device saturates), QD1 latency
# and the rate of fdatasync'ed writes (database commit pattern).
#
# Author: Oleg Zhurko <oleg@zhurko.de>
# SPDX-License-Identifier: MIT
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
. "$HERE/../lib/common.sh"
# shellcheck source=../lib/hw.sh
. "$HERE/../lib/hw.sh"

ALL_TESTS="seqread,seqwrite,randread,randwrite,randrw,latency,syncwrite"
DEFAULT_TESTS="seqread,seqwrite,randread,randwrite,latency,syncwrite"

usage() {
  cat <<EOF
Usage: disk/run.sh [options]

Tests a file on the file system of --dir (default: current directory) with
O_DIRECT, or a raw block device with --device (destroys its data).
For every throughput test the queue depth is doubled (1 job) and then the
number of jobs is doubled until the result stops improving by >3%; the
best configuration is re-measured for the final number.

Options:
  -d, --dir DIR          directory on the file system to test (default: \$PWD)
      --device DEV       raw block device, e.g. /dev/nvme1n1 (needs --destructive)
      --destructive      allow writing to --device (ALL DATA ON IT IS LOST)
  -y, --yes              do not ask for confirmation
  -s, --size SIZE        test file / region size (default: 4G, quick 1G, long 16G;
                         at most half of the free space; 'all' = whole device)
  -t, --tests LIST       $ALL_TESTS
                         (default: $DEFAULT_TESTS)
      --engine NAME      fio ioengine (default: io_uring, else libaio, posixaio, psync)
      --no-tune          skip the search, use jobs=1 qd=32 (seq) / jobs=4 qd=32 (random)
      --max-jobs N       upper bound for the job search (default: min(CPUs, 32))
      --max-qd N         upper bound for the queue depth search (default 256)
      --probe SEC        runtime of each search probe (default 5, quick 2, long 10)
      --runtime SEC      runtime of the final measurements (default 20, quick 5, long 60)
      --keep             keep the test file
$COMMON_OPTIONS_HELP
EOF
}

opt_dir="" opt_device="" opt_destructive=0 opt_yes=0 opt_size="" opt_tests=$DEFAULT_TESTS
opt_engine="" opt_tune=1 opt_maxjobs="" opt_maxqd=256 opt_probe="" opt_runtime="" opt_keep=0
while (( $# )); do
  case $1 in
    -d|--dir) req_arg "$1" $#; opt_dir=$2; shift 2 ;;
    --device) req_arg "$1" $#; opt_device=$2; shift 2 ;;
    --destructive) opt_destructive=1; shift ;;
    -y|--yes) opt_yes=1; shift ;;
    -s|--size) req_arg "$1" $#; opt_size=$2; shift 2 ;;
    -t|--tests) req_arg "$1" $#; opt_tests=$2; shift 2 ;;
    --engine) req_arg "$1" $#; opt_engine=$2; shift 2 ;;
    --no-tune) opt_tune=0; shift ;;
    --max-jobs) req_arg "$1" $#; opt_maxjobs=$2; shift 2 ;;
    --max-qd) req_arg "$1" $#; opt_maxqd=$2; shift 2 ;;
    --probe) req_arg "$1" $#; opt_probe=$2; shift 2 ;;
    --runtime) req_arg "$1" $#; opt_runtime=$2; shift 2 ;;
    --keep) opt_keep=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) n=0; common_opt "$@" || n=$?; (( n )) || die "unknown option: $1 (see --help)"; shift "$n" ;;
  esac
done
for t in ${opt_tests//,/ }; do
  [[ ",$ALL_TESTS," == *",$t,"* ]] || die "unknown test '$t' (available: $ALL_TESTS)"
done

suite_begin disk "Disk"
have fio || skip "fio is not installed - install it with: $(install_hint fio)"

PROBE_T=${opt_probe:-$(ptime 5 2 10)}
FINAL_T=${opt_runtime:-$(ptime 20 5 60)}
RAMP=$(pick 1 2 3)
NCPU=$(hw_ncpus)
MAXJOBS=${opt_maxjobs:-$(( NCPU < 32 ? NCPU : 32 ))}
QMAX=$opt_maxqd
# fio allocates jobs x iodepth x bs of IO buffers: keep that below 1/4 of RAM
MEM_LIMIT=$(( $(hw_mem_avail) / 4 ))
(( MEM_LIMIT > 4294967296 )) && MEM_LIMIT=4294967296

# ------------------------------------------------------------------ target

DIR="" TARGET=""
if [[ -n $opt_device ]]; then
  [[ -b $opt_device ]] || die "$opt_device is not a block device"
  (( opt_destructive )) || die "testing $opt_device writes over its contents; add --destructive if that is really intended"
  mnt=$(lsblk -n -o MOUNTPOINT "$opt_device" 2>/dev/null | grep -v '^$' | paste -sd' ' -)
  [[ -z $mnt ]] || die "$opt_device (or a partition on it) is mounted at: $mnt"
  holders=$(ls "/sys/class/block/$(basename "$(readlink -f "$opt_device")")/holders" 2>/dev/null)
  [[ -z $holders ]] || die "$opt_device is in use by: $holders (LVM, RAID or dm-crypt)"
  if (( ! opt_yes )); then
    [[ -t 0 ]] || die "refusing to overwrite $opt_device without confirmation (use --yes)"
    read -r -p "All data on $opt_device will be destroyed. Type 'yes' to continue: " answer
    [[ $answer == yes ]] || die "aborted"
  fi
  TARGET=$opt_device
  devsize=$(blockdev --getsize64 "$TARGET" 2>/dev/null || lsblk -b -d -n -o SIZE "$TARGET")
  if [[ $opt_size == all ]]; then SIZE=$devsize
  else
    SIZE=$(parse_size "${opt_size:-$(pick 1G 4G 16G)}") || die "bad --size '$opt_size'"
    (( SIZE > devsize )) && SIZE=$devsize
  fi
else
  DIR=$(cd "${opt_dir:-$PWD}" 2>/dev/null && pwd) || die "directory '${opt_dir:-$PWD}' does not exist"
  [[ -w $DIR ]] || die "no write permission in $DIR"
  TARGET="$DIR/.bench-fio-$$.dat"
  (( opt_keep )) || on_exit "rm -f $(printf %q "$TARGET")"
  [[ $opt_size == all ]] && die "--size all is only valid with --device"
  SIZE=$(parse_size "${opt_size:-$(pick 1G 4G 16G)}") || die "bad --size '$opt_size'"
  free=$(( $(df -Pk "$DIR" | awk 'NR == 2 { print $4 }') * 1024 ))
  if (( SIZE > free / 2 )); then
    SIZE=$(( free / 2 / 1048576 * 1048576 ))
    warn "only $(human_bytes "$free") free in $DIR: test file reduced to $(human_bytes "$SIZE")"
  fi
fi
SIZE=$(( SIZE / 1048576 * 1048576 ))
(( SIZE >= 268435456 )) || die "test size $(human_bytes "$SIZE") is too small (need at least 256 MiB)"

# ------------------------------------------------------- device details

FSTYPE=raw
describe_target() {
  local src fs d model rota tran sched hwq size disks=""
  if [[ -b $TARGET ]]; then
    src=$TARGET
  else
    src=$(findmnt -n -o SOURCE --target "$DIR" 2>/dev/null | head -1)
    fs=$(findmnt -n -o FSTYPE --target "$DIR" 2>/dev/null | head -1)
    FSTYPE=${fs:-unknown}
  fi
  src=${src%%\[*} # btrfs: /dev/sda2[/@home]
  step "target: $TARGET ($FSTYPE on ${src:-?}), size $(human_bytes "$SIZE")"
  sysinfo_set disk_target "$TARGET"
  sysinfo_set disk_fs "$FSTYPE"
  sysinfo_set disk_source "${src:-?}"
  [[ -b $src ]] && disks=$(lsblk -n -s -l -o NAME,TYPE "$src" 2>/dev/null | awk '$2 == "disk" { print $1 }' | sort -u)
  for d in $disks; do
    model=$(lsblk -d -n -o MODEL "/dev/$d" 2>/dev/null | sed 's/[[:space:]]*$//')
    rota=$(cat "/sys/block/$d/queue/rotational" 2>/dev/null)
    tran=$(lsblk -d -n -o TRAN "/dev/$d" 2>/dev/null | tr -d ' ')
    sched=$(sed -n 's/.*\[\(.*\)\].*/\1/p' "/sys/block/$d/queue/scheduler" 2>/dev/null)
    hwq=$(find "/sys/block/$d/mq" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    size=$(lsblk -d -n -o SIZE "/dev/$d" 2>/dev/null | tr -d ' ')
    step "device: /dev/$d ${model:-(no model)} $size rotational=${rota:-?} transport=${tran:-?} scheduler=${sched:-?} hw_queues=$hwq"
    sysinfo_set "disk_dev_$d" "${model:-?} $size rotational=${rota:-?} transport=${tran:-?} scheduler=${sched:-?} hw_queues=$hwq"
  done
  case $FSTYPE in
    tmpfs|ramfs) warn "$DIR is on $FSTYPE (RAM): this measures memory, not storage" ;;
    overlay|aufs) warn "$DIR is on a container $FSTYPE file system; use --dir on a real volume" ;;
    nfs*|cifs|smb*|fuse*|9p|virtiofs) warn "$DIR is on $FSTYPE: results include the network / user space file system" ;;
    zfs) warn "ZFS caches in the ARC and may ignore O_DIRECT (before OpenZFS 2.3): numbers can be inflated" ;;
    btrfs) step "note: btrfs copy-on-write and checksums lower random write results" ;;
  esac
}
describe_target

# --------------------------------------------- O_DIRECT and ioengine probes

fio_quick() { # fio_quick FILE ENGINE DIRECT RW - true if a 1 s job runs cleanly
  fio --name=probe --filename="$1" --size=16M --rw="$4" --bs=4k --ioengine="$2" --direct="$3" \
      --iodepth=4 --runtime=1 --time_based --output-format=terse --terse-version=3 \
      > "$RAW_DIR/fio-probe.txt" 2>&1 &&
    awk -F';' '$1 == "3" && $5 == 0 { ok = 1 } END { exit !ok }' "$RAW_DIR/fio-probe.txt"
}

if [[ -b $TARGET ]]; then
  probe_file=$TARGET probe_rw=randread
else
  probe_file="$DIR/.bench-fio-probe-$$" probe_rw=randrw
  on_exit "rm -f $(printf %q "$probe_file")"
fi
DIRECT=1
if ! fio_quick "$probe_file" psync 1 "$probe_rw"; then
  fio_quick "$probe_file" psync 0 "$probe_rw" ||
    die "fio cannot run in $DIR: $(grep -m1 -i 'error\|fio:' "$RAW_DIR/fio-probe.txt")"
  DIRECT=0
  warn "O_DIRECT is not supported on $FSTYPE: using buffered IO (the page cache inflates read results)"
fi
ENGINE=""
engines=$(fio --enghelp 2>/dev/null)
for e in ${opt_engine:-io_uring libaio posixaio psync}; do
  grep -qw -- "$e" <<< "$engines" || { step "ioengine $e: not built into this fio"; continue; }
  if fio_quick "$probe_file" "$e" "$DIRECT" "$probe_rw"; then ENGINE=$e; break; fi
  step "ioengine $e: not usable here (kernel, seccomp or permissions)"
done
[[ -n $ENGINE ]] || die "no usable fio ioengine"
[[ -b $TARGET ]] || rm -f "$probe_file"
SYNC_ENGINE=0
case $ENGINE in psync|sync|pvsync|pvsync2|vsync) SYNC_ENGINE=1; QMAX=1 ;; esac
step "fio $(fio --version 2>/dev/null), ioengine=$ENGINE, direct=$DIRECT, probes ${PROBE_T}s (+${RAMP}s ramp), finals ${FINAL_T}s"
sysinfo_set disk_fio "$(fio --version 2>/dev/null) ioengine=$ENGINE direct=$DIRECT"

# -------------------------------------------------------------- fio runner

# fio_run TAG RW BS JOBS QD RUNTIME [EXTRA FIO ARGS...]
# prints: r_iops r_bw_kib w_iops w_bw_kib r_lat_us r_p99_us w_lat_us w_p99_us
fio_run() {
  local tag=$1 rw=$2 bs=$3 jobs=$4 qd=$5 rt=$6 per out bsb
  shift 6
  bsb=$(parse_size "$bs")
  if (( jobs * qd * bsb > MEM_LIMIT )); then
    step "skipping jobs=$jobs qd=$qd: IO buffers would exceed $(human_bytes "$MEM_LIMIT")" >&2
    return 1
  fi
  local size_args=(--size="$SIZE")
  if [[ $rw == read || $rw == write ]] && (( jobs > 1 )); then
    per=$(( SIZE / jobs / 1048576 * 1048576 )) # every job streams through its own region
    size_args=(--size="$per" --offset_increment="$per")
  fi
  out="$RAW_DIR/fio-$tag-j$jobs-q$qd.txt"
  fio --name="$tag" --filename="$TARGET" --rw="$rw" --bs="$bs" --numjobs="$jobs" \
      --iodepth="$qd" --ioengine="$ENGINE" --direct="$DIRECT" "${size_args[@]}" \
      --time_based --runtime="$rt" --ramp_time="$RAMP" --group_reporting \
      --randrepeat=0 --norandommap --exitall_on_error \
      --output-format=terse --terse-version=3 "$@" > "$out" 2>&1
  awk -F';' '$1 == "3" && NF > 90 {
      if ($5 != 0) exit
      for (i = 18; i <= 37; i++) if ($i ~ /^99\.000000%=/) { split($i, a, "="); r99 = a[2] }
      for (i = 59; i <= 78; i++) if ($i ~ /^99\.000000%=/) { split($i, a, "="); w99 = a[2] }
      print $8, $7, $49, $48, $40, r99 + 0, $81, w99 + 0; ok = 1; exit
    } END { exit !ok }' "$out" && return 0
  warn "fio $tag (jobs=$jobs qd=$qd) failed: $(grep -m1 -iE 'error|fio:' "$out")"
  return 1
}

# metric NAME "fio_run output" - the value a test is tuned for
metric() {
  local f
  read -ra f <<< "$2"
  case $1 in
    r_iops) echo "${f[0]}" ;;
    w_iops) echo "${f[2]}" ;;
    rw_iops) awk -v a="${f[0]}" -v b="${f[2]}" 'BEGIN { printf "%.2f\n", a + b }' ;;
    r_bw) echo "${f[1]}" ;;
    w_bw) echo "${f[3]}" ;;
  esac
}

fmt_metric() { # fmt_metric NAME VALUE
  case $1 in
    *_bw) awk -v v="$2" 'BEGIN { printf "%.1f MB/s", v * 1024 / 1e6 }' ;;
    *) awk -v v="$2" 'BEGIN { printf "%.0f IOPS", v }' ;;
  esac
}

# --------------------------------------------------------------- the tests

TUNE_JOBS=1 TUNE_QD=1
# tune TAG RW BS METRIC [EXTRA...] - finds the jobs x queue depth optimum
tune() {
  local tag=$1 rw=$2 bs=$3 m=$4 q j v out best=0 bj=1 bq=1 miss
  shift 4
  probe() { # probe JOBS QD [EXTRA...] - sets out and v of tune()
    local pj=$1 pq=$2
    shift 2
    out=$(fio_run "$tag" "$rw" "$bs" "$pj" "$pq" "$PROBE_T" "$@") || return 1
    v=$(metric "$m" "$out")
    step "$(printf 'jobs=%-3s qd=%-4s %s' "$pj" "$pq" "$(fmt_metric "$m" "$v")")"
  }
  # 1) queue depth with a single job (one extra step of patience)
  q=1 miss=0
  while (( q <= QMAX )); do
    probe 1 "$q" "$@" || break
    if better "$v" "$best"; then best=$v bq=$q miss=0
    else miss=$(( miss + 1 )); (( miss >= 2 )) && break; fi
    q=$(( q * 2 ))
  done
  # 2) more jobs at that depth (CPU / submission side parallelism)
  j=2
  while (( j <= MAXJOBS )); do
    probe "$j" "$bq" "$@" || break
    if better "$v" "$best"; then best=$v bj=$j; else break; fi
    j=$(( j * 2 ))
  done
  # 3) deeper queues once several jobs share the device
  if (( bj > 1 )); then
    q=$(( bq * 2 ))
    while (( q <= QMAX )); do
      probe "$bj" "$q" "$@" || break
      if better "$v" "$best"; then best=$v bq=$q; else break; fi
      q=$(( q * 2 ))
    done
  fi
  TUNE_JOBS=$bj TUNE_QD=$bq
}

# tuned_test NAME RW BS METRIC [EXTRA FIO ARGS...]
tuned_test() {
  local name=$1 rw=$2 bs=$3 m=$4 out f bw iops lat
  shift 4
  info "$name: $rw bs=$bs"
  if (( opt_tune )); then
    tune "$name" "$rw" "$bs" "$m" "$@"
  elif [[ $rw == read || $rw == write ]]; then
    TUNE_JOBS=1 TUNE_QD=$(( QMAX < 32 ? QMAX : 32 ))
  else
    TUNE_JOBS=$(( MAXJOBS < 4 ? MAXJOBS : 4 )) TUNE_QD=$(( QMAX < 32 ? QMAX : 32 ))
  fi
  (( SYNC_ENGINE )) && (( ! opt_tune )) && TUNE_JOBS=$(( MAXJOBS < 16 ? MAXJOBS : 16 ))
  step "final: jobs=$TUNE_JOBS qd=$TUNE_QD for ${FINAL_T}s"
  out=$(fio_run "$name-final" "$rw" "$bs" "$TUNE_JOBS" "$TUNE_QD" "$FINAL_T" "$@") || return 1
  read -ra f <<< "$out"
  local cfg="bs=$bs jobs=$TUNE_JOBS qd=$TUNE_QD"
  case $m in
    r_bw|w_bw)
      bw=$(metric "$m" "$out")
      bw=$(awk -v v="$bw" 'BEGIN { printf "%.1f\n", v * 1024 / 1e6 }')
      lat=$([[ $m == r_bw ]] && echo "${f[4]}" || echo "${f[6]}")
      good "$name: $bw MB/s ($cfg)"
      result disk "$name" "$bw" "MB/s" "$cfg avg_lat=$(fdiv "$lat" 1000 2)ms"
      ;;
    r_iops|w_iops)
      iops=$(metric "$m" "$out")
      bw=$([[ $m == r_iops ]] && echo "${f[1]}" || echo "${f[3]}")
      lat=$([[ $m == r_iops ]] && echo "${f[4]}" || echo "${f[6]}")
      good "$name: $(fmt_metric "$m" "$iops") ($cfg)"
      result disk "$name" "$(fdiv "$iops" 1 0)" "IOPS" \
        "$cfg MB/s=$(awk -v v="$bw" 'BEGIN { printf "%.1f", v * 1024 / 1e6 }') avg_lat=$(fdiv "$lat" 1 1)us"
      ;;
    rw_iops)
      iops=$(metric "$m" "$out")
      good "$name: $(fmt_metric "$m" "$iops") ($cfg)"
      result disk "$name" "$(fdiv "$iops" 1 0)" "IOPS" \
        "$cfg read_iops=$(fdiv "${f[0]}" 1 0) write_iops=$(fdiv "${f[2]}" 1 0)"
      ;;
  esac
}

latency_test() {
  local out f
  info "latency: 4k random IO at queue depth 1, one job"
  if out=$(fio_run lat_read randread 4k 1 1 "$FINAL_T" --lat_percentiles=1); then
    read -ra f <<< "$out"
    good "read:  avg $(fdiv "${f[4]}" 1 1) us, p99 ${f[5]} us"
    result disk lat_read_4k_qd1 "$(fdiv "${f[4]}" 1 1)" "us" "p99=${f[5]}us iops=$(fdiv "${f[0]}" 1 0)"
  fi
  if out=$(fio_run lat_write randwrite 4k 1 1 "$FINAL_T" --lat_percentiles=1); then
    read -ra f <<< "$out"
    good "write: avg $(fdiv "${f[6]}" 1 1) us, p99 ${f[7]} us"
    result disk lat_write_4k_qd1 "$(fdiv "${f[6]}" 1 1)" "us" "p99=${f[7]}us iops=$(fdiv "${f[2]}" 1 0)"
  fi
}

sync_test() {
  local out f
  info "syncwrite: 4k sequential writes, each followed by fdatasync (database commit pattern)"
  out=$(fio_run syncwrite write 4k 1 1 "$FINAL_T" --ioengine=sync --fdatasync=1) || return 1
  read -ra f <<< "$out"
  local per_op
  per_op=$(awk -v i="${f[2]}" 'BEGIN { if (i > 0) printf "%.1f", 1e6 / i; else print 0 }')
  good "$(fdiv "${f[2]}" 1 0) durable writes/s (~$per_op us per write+fdatasync)"
  result disk sync_write_4k "$(fdiv "${f[2]}" 1 0)" "IOPS" "per_write_us=$per_op ioengine=sync fdatasync=1"
}

# ---------------------------------------------------------------- main

if [[ ! -b $TARGET || $opt_destructive == 1 ]]; then
  info "preparing $(human_bytes "$SIZE") of test data (sequential write)"
  fio --name=prep --filename="$TARGET" --size="$SIZE" --rw=write --bs=1M --ioengine="$ENGINE" \
      --iodepth=8 --direct="$DIRECT" --end_fsync=1 --output-format=terse --terse-version=3 \
      > "$RAW_DIR/fio-prep.txt" 2>&1 || die "writing the test data failed: $(tail -3 "$RAW_DIR/fio-prep.txt")"
fi

for t in ${opt_tests//,/ }; do
  case $t in
    seqread) tuned_test seq_read read 1M r_bw ;;
    seqwrite) tuned_test seq_write write 1M w_bw ;;
    randread) tuned_test rand_read_4k randread 4k r_iops ;;
    randwrite) tuned_test rand_write_4k randwrite 4k w_iops ;;
    randrw) tuned_test rand_rw70_4k randrw 4k rw_iops --rwmixread=70 ;;
    latency) latency_test ;;
    syncwrite) sync_test ;;
  esac
done

suite_end disk
