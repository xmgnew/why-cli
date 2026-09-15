# Usage guide

This guide describes the current prototype. Planned interfaces in the
[v0.1 specification](v0.1-spec.md) are not necessarily implemented.

## Build and run

Follow the [README quick start](../README.md#try-it) from the repository root.
Use `./build/why` directly; installing it is not required. Linux needs a readable
`/proc` with modern CPU accounting fields through `guest_nice`.

```text
why sample [--count N] [--details] [--history]
why --help
```

| Option | Behavior |
|---|---|
| `--count N` | Number of samples, including the initial baseline. Default: 2. Range: 1–1000000. |
| `--details` | Display collected arguments, cwd, and executable path with observation time and field status. |
| `--history` | Print retained lifecycle observations after sampling ends. |
| `--help` | Display usage without starting collection. |

Options can appear in any order after `sample`; repeating an option is an error.
Sampling targets one-second deadlines. Delayed scans skip missed deadlines rather
than fabricating samples. For example, ten samples usually span about nine seconds,
plus collection time. `--count 1` cannot produce an adjacent-sample CPU rate.

History is collected with or without `--history`; that flag controls the timeline
printed at the end. Ctrl-C or SIGTERM requests a graceful stop. There is no
cross-terminal access, recording-file format, or restart recovery yet.

## Reading CPU output

Each sample includes scan duration, visible entry count, whole-machine CPU, scan
quality counters, and a diagnostic process table. Readable processes appear in
PID order; this is not a CPU contributor ranking.

| Field | Meaning |
|---|---|
| `PID` / `PPID` | Process ID and parent PID observed during this scan. |
| `START_TICKS` | Kernel process start time in clock ticks since boot; used with PID for identity. |
| `CPU_SECONDS` | User + system CPU consumed between this process's adjacent valid reads. |
| `CORES` | CPU seconds divided by the actual elapsed interval. Values can exceed 1. |
| `STATE` | Kernel process state character, such as `R`, `S`, or `Z`. |
| `PARENT_ID` | Resolved parent PID and start ticks, or `unknown` if unavailable. |
| `COMMAND` | Observed process name, with control bytes escaped and truncation marked. |

The system busy percentage includes user, nice, system, IRQ, and softirq time.
Idle, I/O wait, and steal time are excluded from busy. Guest time is not added a
second time. High busy percentage alone does not prove a slowdown or CPU saturation.
Process totals are not guaranteed to reconcile exactly with the system measurement.

Rates require adjacent observations of the same identity, increasing counters,
and a valid elapsed interval of 0.5–1.5 seconds. The first sample, a reused PID,
a failed prior read, or a timing/counter discontinuity can produce `n/a`.
One process's measurement interval is not exactly aligned with every other process;
time alignment for incident attribution is still planned.

## Reading history

The final summary reports retained frames, time span, charged bytes, evicted frames,
and observations that could not fit in lifecycle tracking. These are actual retained
values, not a promise of a complete five-minute record.

With `--history`, each lifecycle row contains a monotonic-time interval, a process
identity (`PID:start_ticks`), and an observation type:

| Observation | Interpretation |
|---|---|
| `first seen` | The recorder first successfully observed this identity within its tracking knowledge. It may have started earlier. |
| `visibility lost` | The identity could not be read, or was absent from an incomplete scan. This does not establish exit. |
| `visibility restored` | The same identity became readable again. |
| `no longer observed` | A complete scan did not find the identity, its proc entry vanished, or the PID was reused. The interval bounds the observation loss, not an exact exit time. |
| `metadata changed` | Available observations show a context or process-name change. It is not a complete exec trace. |

Timestamps are monotonic seconds, not wall-clock dates. Clock gaps are marked.
A history window may include gaps or partial scans. Expiry and memory pressure
evict whole old snapshots. Events from before the retained window are unavailable.

## Context and privacy

Context is read on first observation and refreshed after ten seconds or an observed
process-name change. A stat recheck prevents attaching context to a different PID
incarnation. Reads across different files are still not an atomic snapshot.

Each metadata field retains at most 4096 bytes, with an explicit truncation flag.
Command-line bytes preserve argument separators, displayed as `\x00`; output is
not a shell command to copy and execute. Other control bytes are also escaped.
Empty fields, missing paths, permission failures, and failed identity checks are
reported separately. Cached context is labeled with its observation time.

Arguments are collected even without `--details`, but remain hidden in default
output. The recorder does not read the process environment or transmit collected
data. Shell redirection can persist output, so review it before sharing logs,
especially when `--details` is enabled.

## Troubleshooting

| Message or symptom | What to check |
|---|---|
| Live collection requires Linux | macOS supports fixture tests only. Run live sampling on Linux. |
| Cannot collect `/proc` | Check procfs availability and access in the current environment. Unsupported or malformed system input is rejected. |
| Permission-denied fields | Some process information is restricted. Available CPU observations still remain useful; missing context is not fabricated. |
| Scan exceeds 200 ms | Timing quality is degraded. A slow VM or many processes can increase scan time; this is not a demonstrated performance target. |
| Partial scan or skipped entries | Collection reached a bound or enumeration failed. Absence from that scan is not treated as an exit. |
| Metadata budget reached | Context has its own per-frame cap. CPU collection can continue with unavailable context. |
| Cannot retain sample | The configured record budget cannot hold the snapshot, or allocation failed. Collection stops rather than growing history without a bound. |
| Much less than five minutes retained | The command ran for less time, a suspend expired old data, or the memory quota evicted snapshots. |
| No spike explanation | Detection and attribution are not implemented yet. The current output is a diagnostic record. |

Exit status is 0 on normal completion or graceful stop, 1 on a runtime failure,
and 2 for invalid command-line usage.
