# why

Understand what your computer just did.

A CPU burst can be over before you open a process monitor. `why` is being built
as a local recorder that keeps recent process activity and, eventually, explains
which processes contributed to an increase in CPU use.

**Status: early Linux prototype.** Process sampling, metadata, lifecycle
observations, and bounded history work today. Spike detection and contributor
ranking are the next milestones. This is not yet the complete v0.1 experience.

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
./build/why sample --count 10 --history
```

This takes ten samples at a nominal one-second cadence, prints process CPU
measurements, and shows retained lifecycle observations on completion. The first
sample establishes counters; CPU rates become available from the second sample.
Ctrl-C stops collection and prints the retained summary.

```sh
./build/why sample --count 30             # Process and CPU samples
./build/why sample --count 30 --history   # Also print the lifecycle timeline
./build/why sample --count 5 --details    # Also display captured arguments and paths
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
- Explicit diagnostics for missing data, partial scans, timing gaps, and truncation.

The recorder runs without root and uses procfs polling. It makes no network
requests, writes no recording files, and has no daemon or external service.
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

The 64 MiB recording budget includes history structures and conservatively charged
metadata references. Collector working buffers have separate limits, so this is
not a total RSS guarantee. Memory pressure can shorten the retained history.

## Road to v0.1

1. **Complete:** process/CPU collection, metadata, lifecycle observations, bounded history.
2. **Next:** align sampling intervals and detect sustained CPU spikes.
3. **Then:** rank total and incremental CPU contributions with explicit uncertainty.
4. **Later in v0.1:** foreground recorder and cross-terminal queries.

`why watch`, `why cpu`, and `why 60s` are planned interfaces, not available commands.
Memory, disk, network, a TUI, and live macOS support are outside the current scope.

## Development

Read [CONTRIBUTING.md](CONTRIBUTING.md) for build, test, and review conventions.
The [implementation guide](docs/implementation.md) describes module ownership,
resource limits, current validation, and remaining work. The detailed
[v0.1 design specification](docs/v0.1-spec.md) is currently written in Chinese.
