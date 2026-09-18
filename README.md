# why

Understand what your computer just did.

A CPU burst can be over before you open a process monitor. `why` is being built
as a local recorder that keeps recent process activity and estimates which
observed processes contributed to an increase in CPU use.

**Status: early Linux prototype.** Process sampling, metadata, lifecycle
observations, bounded history, CPU spike detection, and contributor rankings
work today, including a foreground recorder and cross-terminal queries.
Historical parent chains and conservative lifecycle timing evidence are available;
performance targets and optional query detail expansion remain in development.

## Try it

Requires Linux, a C17 compiler, CMake 3.16 or newer, and a build tool such as Make
or Ninja. There are no third-party runtime library dependencies.

From the repository root:

```sh
mkdir -p build/tmp
export TMPDIR="$PWD/build/tmp"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
(cd build && ctest --output-on-failure)
./build/why watch
```

Leave `watch` running, then open another terminal in the repository:

```sh
./build/why             # Query the last 60 seconds
./build/why cpu 30s     # Query the last 30 seconds
./build/why 300s        # Query up to five minutes of retained history
```

On Linux, both terminals connect through a same-user abstract Unix socket. There
is no runtime directory, socket file or lock file. For multiple independent
recorders, set the same `WHY_SOCKET_NAME` in each matching pair of terminals
(for example `export WHY_SOCKET_NAME=experiment`).

It runs in the
foreground; Ctrl-C stops it and releases all recorded history. Queries report the
actual available window, total CPU rankings and overlapping spike events. Allow
at least eleven samples to establish a baseline, followed by additional samples
to confirm a spike. No spike is required to see total activity.

For the original diagnostic view:

```sh
./build/why sample --count 30 --history   # Samples and lifecycle timeline
./build/why sample --count 5 --details    # Also show captured arguments and paths
./build/why --help
```

Live collection requires Linux. macOS can build the project and run the portable
fixture tests, but does not support live sampling yet. See the
[usage guide](docs/usage.md) for output fields, limits, and troubleshooting.

## What works today

- Whole-machine CPU measurements and per-process CPU seconds / average cores.
- Process identity based on PID and start time, with observed parent identities.
- Immutable snapshots of arguments, working directory, and executable path.
- First-seen, lost/restored visibility, disappearance, and metadata-change observations.
- In-memory history targeting 300 seconds within a 64 MiB recording budget.
- Baseline-based CPU spike summaries with recovery and interruption states.
- Separate total and incremental CPU rankings, with conservative sibling grouping.
- Time-aligned estimates of observed process CPU, without extrapolating missing data.
- Historical parent chains and lifecycle timing evidence, without causal claims.
- Same-user local queries while the foreground recorder continues sampling.
- Explicit diagnostics for missing data, partial scans, timing gaps, and truncation.

The recorder runs without root and uses procfs polling. Queries use a local Unix
socket; there are no internet requests, runtime files, recording files, or permanent
daemon on Linux. The kernel releases the endpoint when its socket references close,
including after process termination.
Arguments are collected in memory but displayed only with `--details`; they may
contain sensitive values. All retained history is released when the command exits.

## What the numbers mean

System CPU is a whole-machine percentage. Process CPU is reported as CPU seconds
and average cores: two cores corresponds to roughly 200% with a single-core
percentage convention. A parent's own CPU does not include its children's CPU.

Polling does not see every short-lived process, cannot establish exact exit times,
and cannot reconstruct activity from before recording started. `n/a` means the
measurement is unavailable, not zero. A process disappearance is an observation,
not proof of when or why it exited.

The 64 MiB budget reserves 48 MiB for history and 16 MiB for attribution analysis.
History charges each shared metadata allocation once, including its accounting
index, and allocates lifecycle events only when observed. Collector working buffers
and socket buffers have separate limits, so this is not a total RSS guarantee.
Memory pressure can shorten the retained history.

## Road to v0.1

1. **Complete:** process/CPU collection, metadata, lifecycle observations, bounded history.
2. **Complete:** sampling interval alignment and sustained CPU spike detection.
3. **Complete:** total and incremental CPU contributor ranking with explicit uncertainty.
4. **Complete:** foreground recorder and cross-terminal CPU queries.
5. **Complete:** bounded historical parent chains and conservative lifecycle correlations.
6. **Baseline measured:** collection/query costs and bounded-history retention;
   the performance targets are not yet validated.
7. **Complete:** shared metadata accounting and exact lifecycle event allocation;
   the 1000-process synthetic scenario now retains 119 seconds, up from 37.
8. **Next:** compact process snapshots further, expand workload measurements and
   add optional query detail expansion.

Memory, disk, network, a TUI, and live macOS support are outside the current scope.

## Development

Read [CONTRIBUTING.md](CONTRIBUTING.md) for build, test, and review conventions.
The [changelog](CHANGELOG.md) records development milestones.
The [benchmark guide](docs/benchmarks.md) documents reproducible measurements and
current retention/performance limits.
The [implementation guide](docs/implementation.md) describes module ownership,
resource limits, current validation, and remaining work. The detailed
[v0.1 design specification](docs/v0.1-spec.md) is currently written in Chinese.
