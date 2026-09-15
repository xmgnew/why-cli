# Implementation guide

## Current status

The process/CPU collector and metadata/history milestones are implemented.
The [v0.1 specification](v0.1-spec.md) remains the target for spike detection,
incremental attribution, and a recorder/query interface. Those parts do not exist
yet, even though the build uses a development version of `0.1.0`.

The command currently available is `why sample`. It provides a diagnostic view
and an optional lifecycle timeline; it does not yet explain a CPU spike.

## Code map

| File | Responsibility |
|---|---|
| `src/why.h` | Internal observation types, validity states, metadata and collector contracts. |
| `src/model.c` | Pure procfs parsers, CPU arithmetic, identity lookup, parent resolution. |
| `src/procfs.c` | Bounded file reads, frame collection, clocks, scan diagnostics. |
| `src/metadata.c` | Context reads, identity rechecks, refresh policy, immutable snapshot ownership. |
| `src/history.h`, `src/history.c` | Retained frames, lifecycle tracking, byte/time eviction, borrowed history views. |
| `src/main.c` | CLI options, sampling cadence, escaped diagnostic output. |
| `tests/test_core.c` | Parser, CPU, identity, clock and collector integration fixtures. |
| `tests/test_history.c` | Metadata, ownership, lifecycle and retention regressions. |
| `tests/linux_lifecycle.sh` | Linux-only live process appearance/disappearance test. |

The pipeline is:

```text
procfs reads → typed observations → retained snapshots + lifecycle observations
                        ↓                            ↓
               adjacent CPU deltas            bounded history
                        └──────── diagnostic CLI ────┘
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
| Recording quota | 64 MiB |

The recording quota includes history structures, both fixed tracking arrays,
event/frame allocations, and metadata reference charges. Shared metadata is
conservatively charged per snapshot reference, so quota eviction can happen before
physical allocation reaches that amount. Oversized snapshots fail explicitly.
The collector's two working arrays, context allocations and read scratch are
separately bounded; the recording quota is not a total-process RSS limit.

System parsing currently requires ten CPU accounting columns through `guest_nice`.
Unsupported or truncated input fails explicitly. These are prototype limits, not
a promise of universal Linux compatibility.

## Validation baseline

At the metadata/history milestone:

- Four CTest entries passed on macOS with AppleClang and ASan/UBSan.
- Five CTest entries passed on an x86_64 Debian VM with Linux 6.12.107,
  GCC 14.2.0, ASan/UBSan, and `-Werror`.
- The Linux test started a controlled process and verified both its first-seen
  and no-longer-observed entries. Test artifacts were removed afterward.
- A separate five-frame live run observed 161 entries per scan with no skipped
  entries or read/parse errors. Scan times were approximately 36–75 ms in that run.

These are completed smoke/regression results, not a repeatable performance study.
Other QEMU runs exceeded the 200 ms scan-quality threshold. The specification's
CPU-overhead and 1000-process targets still require proper benchmarking.
GitHub Actions is configured for Linux GCC/Clang and macOS Clang; its workflow
file alone does not establish that hosted CI has passed.

## Next milestones

1. Align process CPU intervals with system sampling windows while conserving totals.
2. Implement the specified baseline, spike-confirmation and recovery state machine.
3. Rank total and incremental CPU contributions; retain unknown/unaccounted activity.
4. Add the foreground recorder and cross-terminal query interface.

Each stage should add deterministic fixtures before expanding the user-facing
claims. See [CONTRIBUTING.md](../CONTRIBUTING.md) for the development workflow.
