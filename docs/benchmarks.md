# Performance measurements

These tools establish a reproducible baseline, not a claim that the v0.1 performance
targets have been met. They are opt-in developer tools and are not installed with
`why`. They print aggregate JSON to stdout, without process names or arguments.
Normal recording still creates no runtime files on Linux.

## Build

Use a separate Release build without sanitizers for timing:

```sh
mkdir -p build-perf/tmp
export TMPDIR="$PWD/build-perf/tmp"
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
  -DWHY_SANITIZE=OFF -DWHY_BENCHMARKS=ON -DCMAKE_C_FLAGS=-Werror
cmake --build build-perf --parallel 2
```

Sanitizer builds check correctness separately. Enabling `WHY_BENCHMARKS` adds one
small synthetic CTest smoke check; it has no machine-dependent timing threshold.
The default build remains unchanged. All commands below run from the repository
root; redirected results stay in ignored build directories.

## Collection, history and rendering

```sh
./build-perf/why_bench synthetic 1000 360 20 > build-perf/synthetic-1000.json
# Linux only: 30 samples at a nominal one-second cadence, then 20 renders.
./build-perf/why_bench live 30 20 > build-perf/live-idle.json
```

The synthetic mode generates sorted, stable identities and 360 logical one-second
frames without sleeping or reading procfs. Each process has a metadata allocation
of `sizeof(WhyMetadata) + 256` bytes; workers share a parent and executable label.
One in ten identities accumulates one CPU tick per logical second. Metadata is
stable and reused across two alternating working frames, with normal historical
reference ownership. Each distinct allocation is charged once per history, including
its ledger node; lifecycle arrays hold only actual events. This exercises retention, grouping and report computation; it is
not a simulation of filesystem collection, churn or exec costs. Synthetic CPU
accounting is checked against system counters before reporting success.

The live mode uses the production collector, history and query renderer. Collection
and append timings include the initial sample and later metadata refreshes. Absolute
sampling deadlines skip missed periods as in `watch`. It captures missing reads,
partial/slow scans, gaps, skipped entries and signed accounting residuals.

Both modes render a 60-second report repeatedly over frozen retained history after
sampling finishes. These render timings exclude process startup, IPC, terminal I/O
and concurrent collection. They include bounded attribution and context rendering.
A requested 60-second window may have less available history; check retained span.

JSON fields include nearest-rank p50/p95/p99 wall times in milliseconds, process
CPU time during the recording phase, actual retained span and charged bytes,
frame/metadata/bookkeeping allocation breakdowns, distinct metadata allocation
count, evictions, report size and peak RSS for the whole harness run. Linux `ru_maxrss`
and macOS `ru_maxrss` units are normalized to bytes. CPU percentage is relative to
one logical core, not the whole machine, and excludes the later query phase and
initial buffer allocation. Synthetic CPU percentage is deliberately null because
logical recording time is accelerated. Harness bookkeeping also consumes CPU, so
this is a pipeline measurement rather than an exact idle `watch` CPU reading.

Limits: 1..32768 synthetic processes, 2..3600 samples, 1..100 query repetitions.
Some large configurations cannot retain two frames within the normal budget and
fail explicitly. Interrupted or failed measurements return nonzero and do not
publish a completed result.

## End-to-end queries on Linux

Python 3's standard library is sufficient:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/query_latency.py ./build-perf/why \
  > build-perf/ipc-idle.json
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/query_latency.py ./build-perf/why \
  --load cpu > build-perf/ipc-cpu.json
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/query_latency.py ./build-perf/why \
  --load churn > build-perf/ipc-churn.json
```

Each run owns a recorder in a unique socket session, waits for readiness, warms up
for 12 seconds and issues 40 sequential 60-second queries with a 0.5-second delay
after each. `--warmup`, `--queries` and `--interval` can change those settings.
Reported query wall time includes client startup, socket exchange and capturing
stdout. Failed queries are counted and make the tool return nonzero; successful
latency percentiles exclude failures. Observation-age percentiles expose stale
snapshots rather than interpreting successful requests as proof of fresh sampling.

The script reads only its recorder's procfs counters for CPU and sampled RSS.
Recorder CPU during this phase includes serving queries; client, script and workload
CPU are excluded from that counter. RSS is a peak of observations between queries,
not an OS high-water mark. The script holds response text temporarily but does not
save it. It terminates and reaps owned children on normal completion or handled
interrupts; it does not stop an existing recorder.

`cpu` starts one busy Python process. `churn` repeatedly starts `true` and waits
50 ms between completions, so its rate is bounded by spawning cost plus that delay.
These are controlled stress cases, not representative parallel compilations or
a claim of 1000 live processes. Tests and measurements do not change system limits.

## Interpretation and next measurements

Small samples make p99 close to the slowest observation. VM scheduling, emulation,
background work and clock resolution affect results. Compare like-for-like Release
builds and repeat runs on the same hardware; never compare sanitizer timing to a
Release run. Do not use these timing values as CI pass/fail gates.

The specification's provisional target remains: 1000 visible Linux processes at
1 Hz, average recorder CPU below 1% of one core, and collection p95 below 100 ms.
A synthetic process count does not satisfy that target. Physical Linux runs,
controlled 1000-process collection, parallel compilation, longer recordings and
spike-heavy query cases remain necessary before declaring it met.


## Initial baseline: 2026-09-17

[Aggregate JSON results](benchmarks-2026-09-17.json) retain the measurements behind
these rounded figures. The Linux environment was an x86_64 Debian QEMU VM with
8 logical CPUs, Linux 6.12.107+deb13-amd64 and GCC 14.2.0. The macOS synthetic run
used AppleClang 21.0.0.21000334. Both used Release, sanitizers off and `-Werror`.
These are short observations on one VM and one development machine, not portable
performance guarantees.

| 1000-process synthetic history | Append p95 | Render p95 | Retained span | Charged history |
|---|---:|---:|---:|---:|
| macOS | 0.11 ms | 4.85 ms | 37 s | 47.45 MiB |
| Linux VM | 2.67 ms | 44.30 ms | 37 s | 47.45 MiB |

Both runs appended 360 frames but retained only 38 (37 seconds), evicting 322.
This was the original accounting policy operating as designed, but was far from
five minutes for this scenario. Fixed process snapshots and repeated charges for
shared metadata/parent references both consumed budget. The original policy charged
each reference even when the allocation was physically shared. The storage
optimization below replaces that policy; these baseline results remain unchanged.

The live pipeline run saw 183 entries across 30 scans. Collection p50/p95/p99 were
41.76/60.80/60.96 ms; recording CPU was 4.26% of one logical core. There were no
sampling gaps, partial scans or evictions. It retained 29.04 seconds and reported
17.69 MB peak harness RSS. Signed CPU residual was −0.0184 CPU seconds; it was
not rescaled away. This small-process VM run does not validate the 1000-process
CPU target, even though its collection p95 was below 100 ms.

| End-to-end query scenario | Successful/attempted | Query p50 | Query p95 | Query p99 | Recorder CPU during queries |
|---|---:|---:|---:|---:|---:|
| Idle | 40/40 | 40.50 ms | 56.47 ms | 69.10 ms | 6.25% |
| One busy process | 40/40 | 15.27 ms | 20.02 ms | 36.53 ms | 2.89% |
| Bounded churn | 40/40 | 26.56 ms | 36.54 ms | 50.74 ms | 4.52% |
| Idle repeat | 40/40 | 45.96 ms | 54.95 ms | 62.50 ms | 6.74% |

Each query scenario used the default 12-second warmup and 40 queries. A lower
latency in the busy-process run is not evidence that CPU load improves the
recorder: run ordering, guest/host scheduling and other uncontrolled effects can
change these short measurements. No concurrency or worst-case spike-rendering
claim follows from them. Recorder CPU includes query work here, whereas the live
pipeline CPU figure above excludes its later rendering phase.

The extended correctness suite passed 15/15 tests on macOS and 17/17 on Linux with
ASan/UBSan and `-Werror`, including the benchmark smoke case. Separate sanitized
1000-identity/360-frame runs also completed with exact synthetic CPU accounting.
Those sanitizer runs are not included in these timing tables.


## Storage optimization: 2026-09-17

The unchanged 48 MiB history quota now retains 120 frames (119 seconds) in the
1000-process, 360-sample synthetic scenario on both platforms, versus 38 frames
(37 seconds) before. The implementation charges each shared metadata allocation
once per history and allocates lifecycle arrays only for observed events. It
preserves all process records, identities and uncertainty flags. No history is
written to disk during normal recording.

At the end of that scenario, charged history is 50,079,560 bytes: 46,148,160 bytes
of frames/processes/events, 736,000 bytes across 2,000 metadata allocations, and
3,195,400 bytes of bookkeeping. Two working frames each own 1,000 distinct metadata
allocations; identical labels do not merge allocations. Frame storage is about
92% of the total, making process record compaction the next priority. Five-minute
retention is still not achieved here.

More retained history is not a claim of lower RSS or faster queries. The baseline
60-second query had only 37 seconds available; the optimized run has the full
requested window. Append also performs an event-counting pass and maintains the
reference ledger. Allocator overhead, working buffers and query allocations are
outside the history quota.

[Aggregate optimization results](benchmarks-storage-2026-09-17.json) include initial
reruns and sequential old/new comparisons. For the latter, the old variant uses
`src/history.c` and `src/history.h` from commit
`6b16a6aa2b5b86c85fd5cbca6472d2555149f8f0`, with otherwise identical current sources
and workload; its harness omits the new accounting-breakdown fields. Both variants
use the same Release flags. Each platform runs old/new with 30 samples, then
old/new with 360 samples, always 1000 identities and 20 report renders. This is a
single pair per case, without randomized order or statistical speedup claims.

| 360-sample comparison | Append p95 | Render p95 | Retained span | Peak harness RSS |
|---|---:|---:|---:|---:|
| macOS, old | 0.07 ms | 3.92 ms | 37 s | 24.61 MiB |
| macOS, optimized | 0.12 ms | 6.07 ms | 119 s | 55.34 MiB |
| Linux VM, old | 15.51 ms | 275.96 ms | 37 s | 28.69 MiB |
| Linux VM, optimized | 20.33 ms | 440.60 ms | 119 s | 54.43 MiB |

The Linux append median also rises from 4.26 to 11.84 ms in this pair. The change
improves retention at the cost of accounting work; it does not establish a CPU
performance improvement. For the equal-coverage 30-sample cases, both variants
retain 29 seconds and produce matching CPU totals and zero residuals. Linux
append p95 is 27.46/18.61 ms (old/new) and render p95 is 295.54/210.38 ms, illustrating
why short timing samples are insufficient to isolate the cost of each change.

The rebuilt old implementation itself is much slower than the initial baseline
on this VM (render p95 275.96 versus 44.30 ms). Thus comparing this session directly
against the earlier timing table cannot isolate the storage change; environment
and run variability remain uncontrolled. Retention and allocation counts are
deterministic in this synthetic workload and agree across runs.

The new 30-scan live run saw 167 entries, collection p95 163.79 ms, recording CPU
8.64% of one core, and zero reported gaps/partial/slow scans. Its signed residual
was −2.2453 CPU seconds (system 0.34, observed processes 2.5853), retained without
rescaling. This poor agreement and the different visible process count prevent
treating it as a clean comparison with the earlier live run or as validation of
the CPU target. A follow-up with the old implementation also shows a large signed
residual (−2.4860 CPU seconds), collection p95 165.61 ms and recording CPU 9.02%,
with 166–167 entries. The discrepancy is therefore also present without this
storage change; its source has not been isolated. Controlled physical-Linux runs
remain necessary.

The extended ASan/UBSan suites passed 15/15 on macOS and 17/17 on Linux after the
change, plus separate 1000-identity/360-frame sanitizer stress runs. The final
simultaneous-event and rollback fixtures were rerun on both platforms. Sanitizer
timings are excluded from these tables.
