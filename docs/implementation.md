# Implementation guide

## Current status

The collector, metadata/history, interval alignment, CPU spike detection, and
contributor ranking milestones are implemented. The recorder/query interface in
the [v0.1 specification](v0.1-spec.md) is still pending; the build version `0.1.0`
still denotes development work.

The command currently available is `why sample`. It provides a diagnostic view
with an optional lifecycle timeline, CPU spike summaries, and total/incremental
contributor rankings.

## Code map

| File | Responsibility |
|---|---|
| `src/why.h` | Internal observation types, validity states, metadata and collector contracts. |
| `src/model.c` | Pure procfs parsers, CPU arithmetic, identity lookup, parent resolution. |
| `src/procfs.c` | Bounded file reads, frame collection, clocks, scan diagnostics. |
| `src/metadata.c` | Context reads, identity rechecks, refresh policy, immutable snapshot ownership. |
| `src/history.h`, `src/history.c` | Retained frames, lifecycle tracking, byte/time eviction, borrowed history views. |
| `src/cpu_analysis.h`, `src/cpu_analysis.c` | Overlap estimates and the fixed-size median/MAD detector state machine. |
| `tests/test_cpu_analysis.c` | Alignment, confirmation, recovery, gap and history integration fixtures. |
| `src/attribution.h`, `src/attribution.c` | Bounded identity aggregation, baseline rates, sibling grouping, rankings and quality gates. |
| `tests/test_attribution.c` | Total/increment separation, grouping, clipping, unknown baselines and quality regressions. |
| `src/main.c` | CLI options, sampling cadence, escaped diagnostic output. |
| `tests/test_core.c` | Parser, CPU, identity, clock and collector integration fixtures. |
| `tests/test_history.c` | Metadata, ownership, lifecycle and retention regressions. |
| `tests/linux_lifecycle.sh` | Linux-only live process appearance/disappearance test. |

The pipeline is:

```text
procfs reads → typed observations → bounded history + lifecycle observations
                     ↓                     ↓
             adjacent CPU deltas     spike detection
                                           ↓
                              aligned contributor rankings
                     └──────── diagnostic CLI ────────┘
```

Core tests can use synthetic counters or a temporary procfs-shaped directory.
They do not need root, a running recorder, or a specific live process list.

## Collection and identity

The CLI alternates two reusable working frames. Enumeration keeps a bounded heap
of the lowest PIDs after a rotation cursor and wraps on a subsequent scan.
Processes are then sorted by PID for binary-search lookup. Incomplete enumeration
is marked explicitly. Only top-level process CPU is read; thread CPU and
`cutime`/`cstime` are not added again.

An identity is `(pid, starttime_ticks)` within the recording lifetime. Counter
deltas never cross a detected identity change. Every stat read has its own
monotonic time bracket; CPU calculations use actual midpoint-to-midpoint intervals.
CPU sets are compared exactly. Clock gaps, counter regression, and suspend
boundaries remain visible instead of being converted to zero activity.

Parent relationships are snapshots of observed identities. Unknown parents stay
unknown, and old frames retain their original relationships. The name or PID of
a current process is never used to rewrite a different historical identity.

## Metadata ownership

Metadata uses immutable, reference-counted allocations. Processes and historical
parent-context references retain their own references. Reusing a working frame
or evicting an old history frame releases those references; it does not mutate
metadata that is still referenced elsewhere. This is a single-threaded design.

The collector reads arguments, cwd, and executable link text through the same
process-directory FD used for the first stat. A second stat verifies identity.
A failed recheck exposes no mixed context. This proves identity continuity, not
an atomic snapshot of an exec, argument change, or working-directory change.

Each field records its own availability, length and truncation. Arguments retain
embedded NULs. Snapshots refresh after ten seconds or comm changes; the previous
snapshot is retained unchanged. A gap in visibility does not establish a complete
history of metadata changes or executions.

## Lifecycle tracking

The bounded tracker survives individual frame eviction. A missing process in a
partial scan, or a read/permission failure, loses visibility without being declared
gone. Reading the same identity again restores visibility. Confirmed absence or
PID reuse closes the previous observation with a last-seen/first-missing interval.

Tracker saturation preserves existing identities and reports new observations it
could not remember. Both per-frame and cumulative drop counts remain available.
There is no claim of complete process lifecycle coverage.

## Storage and limits

History is a FIFO of compact frame copies and lifecycle observations. Append
requires increasing observation times. Old snapshots are evicted on elapsed-time
expiry or byte pressure; BOOTTIME expiry also covers suspend time. The public
internal history views are borrowed and may become invalid on the next append.

| Resource | Current bound |
|---|---|
| Process entries per working frame | 32768 |
| Lifecycle identities | 32768 in the CLI |
| Supported logical CPU IDs | 0–8191 |
| System stat input buffer | 4 MiB |
| Process stat input buffer | 4095 bytes of input |
| Displayed process name | 255 bytes, then explicit truncation |
| Each context field | 4096 bytes, then explicit truncation |
| Context allocations per working frame | 8 MiB |
| Retention target | 300 seconds |
| History quota | 48 MiB |
| Attribution reservation | 16 MiB, including scratch arrays |
| Attribution identities | 32768 |

The history quota includes history structures, both fixed tracking arrays,
event/frame allocations, and metadata reference charges. Shared metadata is
conservatively charged per snapshot reference, so quota eviction can happen before
physical allocation reaches that amount. Oversized snapshots fail explicitly.
The collector's two working arrays, context allocations and read scratch are
separately bounded; the recording quota is not a total-process RSS limit.

System parsing currently requires ten CPU accounting columns through `guest_nice`.
Unsupported or truncated input fails explicitly. These are prototype limits, not
a promise of universal Linux compatibility.

## CPU analysis milestone

The pure CPU analysis module clips each valid process interval to a requested
window using a uniform-rate assumption. Adjacent disjoint windows conserve the
original CPU seconds. History alignment walks retained adjacent frames and emits
observed portions through an optional visitor. It does not allocate or rank
contributors. It skips discontinuities and reports missing boundaries and quality
limitations. Requests beyond the latest system observation are provisional.

The detector stores at most thirty baseline intervals, one candidate, three
pending recovery intervals and an active aggregate. It contains no pointers into
history. Its storage is included in the history quota. Each completion is copied
into its recording frame; frame eviction removes that completed summary. An
ongoing aggregate survives eviction without pinning old raw data. Alignment is
computed from what is still available, and presentation labels missing prefixes.
Algorithm version 1 implements the thresholds in the specification.

Regression fixtures cover threshold freezing, an enter threshold above 100%,
noisy baselines, bounded baseline history, one-interval pulses, recovery rollback,
interruption, invalid values, overlap conservation, PID reuse, and an event that
outlives its raw history. CPU detection does not use process visibility as proof
of cause. Attribution is implemented separately as described below.

## Attribution milestone

Attribution borrows immutable history and returns owned row/index arrays. Destroy
its result before appending to history or releasing it. A bounded identity hash
table collects process observations; system-bin passes accumulate aligned CPU
and coverage, then deterministic sorts produce sibling groups and both rankings.
Each process's portions are combined within a bin before positive excess is taken.
Scratch arrays and result arrays together must fit the 16 MiB reservation.

Baseline coverage, new-process zero baselines, signed residuals and label gates
follow the [usage guide](usage.md#contributor-rankings). Capacity omissions, missing
boundaries, slow/partial scans, and unknown baselines remain visible. Completed
incident aggregates can outlive raw intervals; ranking totals use only comparable
retained system intervals. No valid interval means unavailable, not zero CPU.

Fixtures cover a constant four-core service versus a three-core increase, new
processes, unknown/partial baselines, same/different parents and executables,
context changes, fractional bin clipping, missing reads, accounting mismatch,
slow/partial scans, baseline gaps, eviction, empty measurements and capacity limits.
Direct parent identity is shown; ancestor rollups and automatic lifecycle-based
explanations are still pending. Performance targets have not been benchmarked.

## Validation

Latest completed validation at the attribution milestone (2026-09-15):

| Environment | Result | Checks |
|---|---|---|
| macOS / AppleClang | 6/6 CTest entries passed | ASan and UBSan |
| x86_64 Debian VM / GCC 14.2.0 | 7/7 CTest entries passed | ASan, UBSan and `-Werror` |

The portable suite covers core parsing/arithmetic, history ownership, CPU analysis,
attribution, CLI help and invalid arguments. Linux also runs a controlled live
process-lifecycle test. Live smoke runs checked total rankings and the single-sample
unavailable case. Formatting and repository-local Markdown links were checked.
See [CONTRIBUTING.md](../CONTRIBUTING.md#build-and-test) to reproduce the suite.

These results are regression and smoke checks, not a performance study. QEMU runs
can exceed the 200 ms scan-quality threshold. The CPU-overhead and 1000-process
targets still require benchmarking. GitHub Actions is configured for Linux
GCC/Clang and macOS Clang; hosted CI results have not been verified here.

## Remaining work

1. Add the foreground recorder and cross-terminal query interface. `why watch`,
   `why cpu` and `why 60s` remain planned commands.
2. Expand contextual explanations, including ancestor views and lifecycle timing
   correlations, while preserving polling uncertainty.
3. Benchmark recording overhead and retention under larger process populations.

There is no persistent recording format or restart recovery. The current build
is a development checkpoint, not a completed v0.1 release. Each stage should add
deterministic fixtures before expanding user-facing claims.
