# benchmarks

A collection of server benchmarks that **adapt themselves to the hardware they
run on** to show the highest performance the machine can reach on its
operating system: CPU, memory, storage, network and HTTP request rate.

Instead of fixed parameters, every suite inspects the machine (CPU topology,
SIMD width, cache and RAM sizes, NUMA layout, storage device, network
interface, open-file limits) and then **searches** for the configuration that
saturates the hardware: queue depth × jobs for the disk, parallel streams for
the network, CPU split × connections for HTTP, thread counts and store
strategies for memory, kernel shapes for floating point. The best
configuration is measured again for the final number, and every result is
written to JSON and Markdown together with the configuration that produced it.

```
./bench.sh --install-deps      # once: compiler, fio, iperf3, ... (uses sudo)
./bench.sh --quick             # ~6 minutes: a first look
./bench.sh                     # ~15-25 minutes: the normal run
```

## What is measured

| Suite | Tests | Adapts to the hardware by |
|---|---|---|
| `cpu` | integer ALU (Gops/s), FP64 and FP32 FMA peak (GFLOPS), SHA-256 (MB/s), sort (Melem/s); 1 thread and all CPUs. With sysbench, 7-Zip, OpenSSL installed also their reference numbers | compiling with `-march=native`; choosing the fastest FMA kernel shape (vector width 128/256/512 bit × 8–24 independent accumulators) for each mode; one pinned thread per CPU; the single-thread run on the fastest core (hybrid / big.LITTLE aware) |
| `memory` | read, write, copy, triad bandwidth (GB/s); L1, L2, L3, DRAM latency (ns) | arrays ≥ 4× total last-level cache (capped by free RAM); NUMA-local first touch; threads spread over NUMA nodes and physical cores; thread count swept; every kernel tried with SIMD loops, non-temporal stores (x86) and libc `memset`/`memcpy`; latency working sets derived from the cache sizes, transparent huge pages to keep TLB misses out |
| `disk` | sequential read/write (MB/s), 4K random read/write (IOPS), 4K QD1 latency (µs), fdatasync'ed writes/s; optional 70/30 random mix | detecting file system and device (NVMe/SATA/HDD, scheduler, hardware queues); best `ioengine` (io_uring → libaio → posixaio → psync) and O_DIRECT support; **hill-climbing queue depth, then job count, then depth again** until the result stops improving; test size from free space |
| `network` | TCP upload, download, bidirectional (Gbit/s, % of link speed), RTT; optional UDP | sweeping parallel streams spread over several iperf3 processes (so old single-threaded iperf3 scales too); trying zero-copy; loopback when no peer is given |
| `http` | requests/s and latency for 4 scenarios: `plain` (Hello World), `pipelined` (16 requests in flight per connection), `json` (mock data serialized per request), `compute` (chained SHA-256 per request) | a shared-nothing epoll server (one `SO_REUSEPORT` listener + event loop per pinned core) and a closed-loop load generator, both C; on one machine server and load generator get separate physical cores, several **core splits** are tried (a CPU-bound handler wants more server cores) and **connections are doubled** until the rate stops improving |

`sysinfo` is collected for every run: CPU model, cores/threads/sockets/NUMA,
SIMD, caches, frequency governor, turbo, memory size and DIMMs (root +
`dmidecode`), THP, disks, NICs, OS, kernel, virtualization, cgroup CPU quota,
compiler.

## Requirements

- Linux, `bash` 4.2 or newer (tested with 5.2), coreutils, `awk`, util-linux
  (`lscpu`, `taskset`, `lsblk`, `findmnt`)
- a C compiler (`gcc` or `clang`); the C tools are built on first use for the
  exact CPU (`build/<arch>-<host>/`) and rebuilt when sources or compiler change
- `fio` for `disk`, `iperf3` for `network`
- optional: `sysbench`, `7z`/`7zz`, `openssl` (CPU reference numbers), `wrk`
  (alternative HTTP load generator), `dmidecode` (DIMM details), `ping`

`./bench.sh --install-deps` installs everything with apt, dnf/yum, zypper,
pacman or apk (optional tools are skipped where a distribution lacks them).
A suite whose tool is missing is reported as *skipped* and the others still
run.

Tested on Ubuntu 24.04 (x86_64, gcc 13 and clang 18). The C code is also
built and functionally tested for aarch64 (cross-compiled, run under QEMU).
The package lists for the other package managers are best effort.

## Usage

```
./bench.sh [options] [suite ...]          # suites: sysinfo cpu memory disk network http
```

| Option | |
|---|---|
| `--quick`, `--long`, `--profile P` | run profile: `quick` (short probes, fewer repetitions), `normal` (default), `long` (about 3× longer, steadier numbers) |
| `--disk-dir DIR` | directory whose file system the disk suite tests (default: current directory) |
| `--net-host HOST` | measure the network against `network/run.sh server` running on HOST (default: loopback) |
| `--http-url URL` | load a remote `http/run.sh server` (default: server and load generator on this machine) |
| `--tune` | set the `performance` CPU frequency governor for the run and restore the previous one afterwards (root) |
| `--install-deps` | install the required packages first |
| `-o DIR` | results directory (default `results/<date>-<host>/`) |

On a 4-vCPU cloud VM the quick profile takes about 6 minutes and the normal
profile about 14 (cpu 3 min including the reference tools, memory 40 s, disk
7 min, network 50 s, http 2 min). Larger machines need more search steps
(more thread counts, jobs and connections), so plan for 15–25 minutes.

Examples:

```bash
./bench.sh cpu memory                       # only these suites
./bench.sh --long --disk-dir /mnt/nvme      # long run, disk test on /mnt/nvme
sudo ./bench.sh --tune                      # performance governor during the run
```

Every suite is also a standalone script with more options (`--help`):

```bash
cpu/run.sh -t fp64,fp32 -d 10                 # selected CPU tests, 10 s each
memory/run.sh --threads 1,16,32 --no-latency  # bandwidth at given thread counts
disk/run.sh --dir /data --size 32G --tests randread,randwrite
disk/run.sh --device /dev/nvme1n1 --destructive   # raw device, DESTROYS ITS DATA
network/run.sh --udp                          # loopback incl. UDP
http/run.sh -s json --json-items 100          # bigger mock payloads
http/run.sh --loadgen wrk                     # cross-check with wrk
```

### Two machines: network and HTTP

The network and HTTP suites measure one machine by default (loopback / server
and load generator side by side). To measure the NIC and the network path, or
the server alone, start the server part on one machine and the client part on
the other:

```bash
# machine A (server)                      # machine B (client)
network/run.sh server                     network/run.sh client <A>        # or: ./bench.sh --net-host <A> network
http/run.sh server                        http/run.sh client http://<A>:8080   # or: ./bench.sh --http-url http://<A>:8080 http
```

`network/run.sh server` starts several iperf3 server processes on consecutive
ports (5201…) and prints the matching client command. For the HTTP test the
load generator machine should be at least as strong as the server, otherwise
it measures the load generator.

## How the auto-tuning works

**CPU** – `cpubench` contains the FP kernels in 4–12 shapes (vector width ×
number of independent FMA chains, limited to what fits in the register file).
FMA latency × number of FMA units decides how many independent chains a core
needs; running all shapes briefly and keeping the fastest gives the peak for
any micro-architecture without a table of CPU models. The integer test uses
one-instruction add/rotate/xor steps in 10 independent chains so that Gops/s
reflects ALU issue width rather than code generation.

**Memory** – STREAM-style kernels with STREAM byte accounting. Arrays are
`max(4 × total LLC, 256 MiB)` each (never more than half the free RAM). Each
thread first-touches the part of the arrays it later uses, so pages are on
its NUMA node. Threads are placed one per physical core round-robin over NUMA
nodes before SMT siblings are used, and the sweep (1, 2, 4, … cores, all CPUs)
finds the point of maximum bandwidth, which is often below all cores. For
write/copy/triad, non-temporal stores avoid the read-for-ownership traffic of
normal stores; the fastest method is reported with the result. Latency is a
dependent pointer chase through a random cyclic permutation of 64-byte lines
(Sattolo), immune to hardware prefetchers.

**Disk** – a test file (default 4 GiB, at most half the free space) is first
written completely, so reads never hit unallocated extents. For each
throughput test: double the queue depth with one job (allowing one step
without improvement), then double the number of jobs (more submitting CPUs,
needed for fast NVMe), then try deeper queues again; a step must gain >3%.
Sequential jobs stream through separate regions. The best configuration is
then run for the final measurement (20 s). QD1 latency and fdatasync'ed
writes use fixed settings because their configuration is the point of the
test.

**Network** – stream counts 1, 2, 4, … are tried until throughput stops
growing; streams are spread over up to 8 iperf3 processes and the result is
the sum of what the receivers counted. Zero-copy (`-Z`) is kept if it helps.

**HTTP** – the server (`httpd`) runs one pinned worker per CPU, each with its
own listening socket and epoll loop, keep-alive, pipelining and one `write`
for all responses of a read. The load generator (`httpload`) keeps N
connections busy (closed loop like wrk), records latency into an HDR-style
histogram and reports errors; a probe with more than 1% errors ends the
search. In local mode the server gets 50/67/75/88% of the physical cores
(SMT siblings never split), the connection count is doubled until the rate
stops improving, and the best split and count are measured for 10 s.

Endpoints of `httpd` (usable with any other load generator, too):

| Endpoint | Response |
|---|---|
| `/plain` | `Hello, World!` |
| `/json?items=N` | `{"page":..,"count":N,"items":[...]}` with N mock users (id, name, email, age, active, balance, score, city, tags, created), default 20 ≈ 3.9 KB, serialized per request |
| `/compute?rounds=N[&seed=S]` | `{"rounds":N,"seed":S,"sha256":"..."}`, N chained SHA-256 (default 1000) |

## Results

Each run writes a directory `results/<date>-<host>/`:

| File | Content |
|---|---|
| `summary.md` | system description and a results table, ready to paste |
| `results.json` | `system` properties and `results` (`suite`, `test`, `value`, `unit`, `details`) |
| `results.tsv`, `sysinfo.tsv` | the same as tab separated values |
| `bench.log` | the complete console output |
| `raw/` | output of every tool invocation (each fio probe, iperf3 JSON, …) |

`details` records how a number was reached, e.g. `bs=4k jobs=4 qd=128`,
`threads=32 method=nt-store` or `conns=512 server_cpus=24 client_cpus=8`.

Example from a 4-vCPU cloud VM (Xeon, AVX-512), quick profile:

```
  cpu      fp64_mt                           312.8  GFLOPS   threads=4 kernel=512bit/24acc
  memory   triad_bw                          58.35  GB/s     threads=4 method=nt-store
  memory   latency_dram                      244.0  ns       working_set=1GiB
  disk     rand_read_4k                    254,468  IOPS     bs=4k jobs=4 qd=128 MB/s=1042.7 avg_lat=2011.1us
  network  loopback_tcp_upload               113.5  Gbit/s   streams=16 processes=4 zerocopy retransmits=93
  http     plain_pipelined               3,273,964  req/s    conns=64 pipeline=16 server_cpus=2 client_cpus=2 p50=269us p99=834us MB/s=445.259
  http     compute                           5,428  req/s    conns=8 pipeline=1 server_cpus=3 client_cpus=1 p50=1.62ms p99=4.54ms MB/s=1.313
```

## Getting the maximum out of a machine

The suites find the best parameters for the current OS configuration; these
settings decide how high that maximum is:

- **CPU frequency**: `performance` governor (`./bench.sh --tune` or
  `cpupower frequency-set -g performance`), turbo enabled, BIOS power profile
  set to performance. `sysinfo` warns about a non-performance governor,
  disabled turbo and cgroup CPU quotas.
- **Quiet machine**: stop other workloads; on VMs, noisy neighbours make
  results vary (use `--long` and compare several runs).
- **Memory**: populate all memory channels; keep transparent huge pages at
  `madvise` or `always`.
- **Disk**: test the device you care about with `--dir` on a file system on
  it, or the raw device with `--device` (destructive). SSDs are fastest when
  fresh/trimmed and slow down under sustained writes; `--long` shows more of
  the sustained behaviour. The disk suite writes a lot: on a fast NVMe drive
  a normal run can write a few hundred GB, so mind the wear of small drives.
- **Network**: test against a peer at least as fast; check MTU (jumbo frames
  for 25G+), `irqbalance` / IRQ affinity, NIC ring sizes (`ethtool -g`) and
  offloads; open the iperf3 ports in the firewall.
- **HTTP**: raise `ulimit -n` (the suite raises the soft limit to the hard
  limit), keep `net.core.somaxconn` ≥ 4096; for remote tests the load
  generator must not be the bottleneck.

## Caveats

- In virtual machines and containers the numbers describe the VM: vCPU
  scheduling, virtual disks and the host's cache sharing apply. Cache sizes
  reported inside VMs may be the host's.
- Loopback network results measure the kernel network stack and memory
  bandwidth, not a NIC.
- `cpubench` SHA-256 is portable scalar code by design (comparable across
  architectures); `openssl speed` shows the hardware-accelerated numbers.
- HTTP numbers are for the included C server — the upper bound for this
  machine and kernel; application frameworks reach a fraction of it.

## Repository layout

```
bench.sh            run all / selected suites, collect results
lib/common.sh       output, options, profiles, results, dependencies, builds
lib/hw.sh           hardware / OS detection and the sysinfo report
lib/c/benchutil.h   C helpers: timing, affinity, topology ordering, caches
lib/c/sha256.h      portable SHA-256 (cpu and http benchmarks)
cpu/                run.sh, cpubench.c
memory/             run.sh, membench.c
disk/               run.sh (fio)
network/            run.sh (iperf3)
http/               run.sh, httpd.c (server), httpload.c (load generator)
```

### Adding a benchmark

Create `<suite>/run.sh`, source `lib/common.sh` and `lib/hw.sh`, call
`suite_begin <suite> "<Title>"`, record numbers with
`result <suite> <test> <value> <unit> "<details>"`, finish with
`suite_end <suite>`, and add the suite name to `ALL_SUITES` in `bench.sh`.
C tools are compiled with `build_tool <name> <source.c>`; print
`RESULT key=value ...` lines from them for easy parsing (see `cpu/run.sh`).
Suites that are not Linux-server specific fit the same structure.

## Author

Oleg Zhurko <oleg@zhurko.de>

## License

MIT, see [LICENSE](LICENSE).
