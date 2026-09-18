# Implementation guide

## Current status

The collector, metadata/history, interval alignment, CPU spike detection, and
contributor ranking and foreground recorder/query milestones are implemented.
The [v0.1 specification](v0.1-spec.md) remains the design target; optional query detail
expansion and full performance validation are pending. Version `0.1.0` denotes development work.

`why watch` records in the foreground; `why`, `why Ns`, and `why cpu [Ns]` query
its retained CPU history from another terminal. `why sample` remains available
for per-sample diagnostics and optional metadata/lifecycle output.

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
| `src/main.c` | CLI options, signals, sampling cadence and recorder lifetime. |
| `src/context.h`, `src/context.c` | Same-snapshot ancestor traversal and conservative lifecycle timing evidence. |
| `tests/test_context.c` | PID reuse, cycles/depth, clock uncertainty, gaps, expiry and escaped context output. |
| `src/report.h`, `src/report.c` | Escaped diagnostic and query presentation to an explicit output stream. |
| `src/ipc.h`, `src/ipc.c` | Filesystem-free Linux endpoint, peer checks, bounded nonblocking request/response I/O. |
| `tests/test_ipc.c` | Endpoint lifetime, request framing, duplicate prevention, disconnects and timeouts. |
| `tests/linux_recorder.sh` | Live watch/query, continued sampling and shutdown integration. |
| `tests/test_core.c` | Parser, CPU, identity, clock and collector integration fixtures. |
| `tests/test_history.c` | Metadata, ownership, lifecycle and retention regressions. |
| `benchmarks/bench.c` | Opt-in synthetic/live collection, retention and frozen-history rendering measurements. |
| `benchmarks/query_latency.py` | Owned recorder/workload lifecycle and end-to-end Linux query measurements. |
| `tests/linux_lifecycle.sh` | Linux-only live process appearance/disappearance test. |

The pipeline is:

```text
procfs reads → typed observations → bounded history + lifecycle observations
                     ↓                     ↓
             adjacent CPU deltas     spike detection
                                           ↓
                              aligned contributor rankings
                     └──── reports to stdout / Unix socket ────┘
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

History is a FIFO of frame copies and lifecycle observations. Every retained frame
still contains full process records, allocated for its actual process count. Append
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
| IPC request buffer | 32 bytes |
| IPC response buffer | 128 KiB per endpoint |
| Simultaneously serviced query connections | 1; listen backlog 4 |
| Full incidents shown per query | Latest 8 overlapping incidents |

The history quota includes history structures, both fixed tracking arrays,
event/frame allocations, and a per-history metadata ledger. The ledger uses
`2 * tracking_capacity` fixed hash buckets and one node per distinct metadata
allocation, keyed by pointer identity. All buckets and nodes count toward the
quota. A shared immutable allocation is charged once while any snapshot in this
history references it, including parent references; equal content in separate
allocations is still charged separately. Multiple histories account independently.

Lifecycle transitions are counted before allocating the exact event array; a
frame without events allocates none. Counting does not commit tracker state.
Append reserves quota and stages ledger references before copying the caller's
frame. Failure rolls those references and reservations back without advancing the
tracker or detector; older frames may already have been evicted. The caller must
own the input throughout append and must not pass a borrowed history frame.
Snapshot eviction removes ledger charges before releasing actual metadata
references, keeping accounting separate from allocation lifetime.

`WhyHistoryStats.bytes` equals `frame_bytes + metadata_bytes + bookkeeping_bytes`
after every append, including failure. Bookkeeping includes the detector, fixed
tracking arrays, ledger buckets and nodes; frame bytes include process records and
actual lifecycle events. Oversized snapshots fail explicitly.
The collector's two working arrays, context allocations and read scratch are
separately bounded, as are the fixed IPC buffers; the recording quota is not a
total-process RSS limit.

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
Parent chains and lifecycle timing evidence are described below. Ancestor CPU
rollups are deliberately excluded. Initial benchmark results are documented in [benchmarks.md](benchmarks.md);
the specification targets have not been validated.

## Context evidence

`context.c` performs no collection, allocation or score mutation. Parent traversal
accepts a borrowed process from a retained frame, finds that exact frame, and only
follows identity-matched parents within it. A fixed eight-pointer result and cycle
checks bound traversal; missing context is not backfilled from another frame.
Results borrow history under the same lifetime rules as attribution results.

Birth estimates convert start ticks through the frame's boot/monotonic bracket,
round bounds outward and include one tick of uncertainty. The estimate must fall
after the beginning of the retained continuous clock segment and before the first
retained successful read. Rise correlation requires the entire estimate within
±2 seconds and no retained clock discontinuity between birth/read and rise.
Disappearance correlation uses only retained `WHY_NO_LONGER_OBSERVED` events for
the exact identity and only recovered incidents; it does not infer an exit time.
These conservative rules may withhold a true relationship when evidence is sparse.

Presentation adds context for at most three leading rows per result and limits
ancestor names to 32 bytes before escaping. Groups identify the representative
member and withhold group-wide lifecycle claims. The fixed IPC response buffer is
128 KiB to accommodate bounded ancestor output; oversized reports still fail
explicitly. Arguments and cwd remain exclusive to `sample --details`.

## Foreground recorder and query transport

The foreground process owns both reusable collection frames and history. Between
absolute sampling deadlines it polls a Unix stream socket. Accepted sockets use
nonblocking I/O; one client is serviced at a time and has a two-second deadline.
A disconnect cannot terminate the process through SIGPIPE. Query rendering borrows
history synchronously, destroys attribution results, then sends owned text from a
fixed 128 KiB buffer. No history pointer survives into asynchronous socket writes.
Rendering can still delay the next sample; missed intervals remain discontinuities.
This is not a latency or throughput guarantee under heavy query load.

Linux uses an abstract Unix socket, with a leading NUL in `sun_path` and an exact
address length excluding the trailing string NUL. Its name is
`why-cli.<effective UID>.<session>`, where `WHY_SOCKET_NAME` selects the session
(default `default`; 1–64 ASCII letters/digits/hyphens/underscores). Bind provides
atomic duplicate prevention without a separate lock. No filesystem socket,
directory or lock is created; the kernel releases the endpoint when all socket
references close. Descriptor close-on-exec prevents retention across exec.

Linux `SO_PEERCRED` verifies the effective UID on both ends. Abstract endpoints
have no filesystem permission protection; other users may attempt to connect or
occupy a predictable name, but a different UID never receives a report or supplies
an accepted response. A bind collision fails explicitly rather than falling back
to a different endpoint. Isolation is per Linux network namespace.

The previous Linux `WHY_RUNTIME_DIR` setting is ignored, and old-version runtime
artifacts are left untouched. See [migration notes](usage.md#foreground-recording-and-cross-terminal-queries).
Portable macOS fixtures retain the pathname/lock implementation with `getpeereid`
and explicitly clean their build-local test directory. Live macOS recording is
not implemented; it needs a separate lifecycle design before support is claimed.
The Linux mechanism follows [unix(7)](https://man7.org/linux/man-pages/man7/unix.7.html).

The internal versioned protocol is one canonical ASCII request, `WHY/1 N\n`,
where N is 1..300. Responses use `WHY/1 OK\n`, bounded report text, and a final
`WHY/1 END\n`; failures use `WHY/1 ERROR\n`. The client waits at most five seconds
for a response, verifies framing, and rejects truncated or oversized replies.
This protocol is internal, not a stable recording or third-party integration API.
No raw C structs, process arguments or cwd fields are transferred.

The requested window begins at query time minus N seconds and ends at the latest
retained system observation. Missing prefixes and observation age are printed.
Window totals use the explicit-window attribution entry point without inventing
a baseline. Overlapping incident results retain their original baseline and full
event boundaries, are labeled separately, and never get summed into window totals.
Only eight latest overlapping incidents are rendered, with omissions counted.

## Validation

Latest completed validation after the context evidence update (2026-09-16):

| Environment | Result | Checks |
|---|---|---|
| macOS / AppleClang | 14/14 CTest entries passed | ASan and UBSan |
| x86_64 Debian VM / GCC 14.2.0 | 16/16 CTest entries passed | ASan, UBSan and `-Werror` |

The portable suite covers core parsing/arithmetic, history ownership, CPU analysis,
attribution, context evidence, IPC, CLI help and invalid arguments. New fixtures cover fractional
query clipping, stale windows, full-event versus query-window totals, endpoint
privacy, malformed requests, duplicate recorders, endpoint restart and slow or
disconnected clients. Linux also runs live lifecycle and recorder/query tests,
including continued sampling between queries and shutdown. Linux IPC fixtures
add SIGKILL/rebind checks and assert that no runtime socket or lock was created. Formatting
of edited C files and repository-local Markdown links were checked.
See [CONTRIBUTING.md](../CONTRIBUTING.md#build-and-test) to reproduce the suite.

Storage fixtures cover shared child/parent metadata, independent histories,
replacement allocations, collision chains, budget/time eviction, two simultaneous
lifecycle events, and failed appends after staged references and eviction. They
check quota totals, reference lifetimes and unchanged tracker/detector state on
failure.

The normal correctness suite remains 14 portable tests and 16 Linux tests. Enabling
`WHY_BENCHMARKS` adds a synthetic smoke check. At the benchmark milestone, the
extended macOS suite passed 15/15 and the extended Linux VM suite passed 17/17 with
ASan/UBSan and `-Werror`. A separate sanitized 1000-identity, 360-frame run checked
metadata ownership and eviction. The same extended suites and sanitized stress
runs passed after the storage optimization; timing measurements used unsanitized Release
builds instead. The Python tool was exercised with idle, CPU and churn workloads.

Initial measurements and limits are in the [benchmark guide](benchmarks.md).
These VM observations do not establish physical Linux performance or the
1000-live-process target. GitHub Actions is configured for Linux GCC/Clang and
macOS Clang; hosted CI results have not been verified here.

## Remaining work

1. Compact historical process records without discarding identities or weakening
   unknown-data semantics. Shared allocation accounting and exact event arrays
   extended the synthetic 1000-process retained span from 37 to 119 seconds under
   the same quota. Full process records still dominate; five minutes remains a target.
2. Repeat measurements on physical Linux and extend to controlled 1000-process
   collection, parallel compilation, longer runs and spike-heavy queries.
3. Add explicit opt-in query metadata expansion with observation timestamps and
   privacy-preserving defaults.

There is no persistent recording format or restart recovery. The current build
is a development checkpoint, not a completed v0.1 release. Each stage should add
deterministic fixtures before expanding user-facing claims.
