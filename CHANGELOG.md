# Changelog

Development checkpoints are recorded here. The project has not published a
completed v0.1 release; the CMake version `0.1.0` is a development version.

## Unreleased

### Foreground recorder and queries

- Add `why watch` and same-user cross-terminal queries: `why`, `why Ns` and
  `why cpu [Ns]`, with a default 60-second window and a range of 1..300 seconds.
- Show actual query coverage, observation age, system average/sampled peak and
  window totals separately from full overlapping-event contributions.
- Add private Unix socket transport with peer checks, a recorder lock, stale
  socket recovery, bounded buffers and connection deadlines.
- Separate report presentation from CLI orchestration; preserve diagnostic
  `why sample` behavior and keep arguments out of query responses.
- Add window/report regressions, IPC fixtures and live Linux recorder/query tests.
- Document runtime setup, shutdown, synchronous rendering limits and pending
  performance work. Edited C files follow the updated Tab formatting preference.

### CPU analysis and attribution

- Align observed process CPU deltas to system intervals without filling missing
  observations with zero or extrapolating process lifetimes.
- Detect sustained whole-machine CPU spikes using a median/MAD baseline, frozen
  thresholds, recovery confirmation and explicit interruption states.
- Print separate total and incremental contributor rankings when sampling ends;
  show retained total activity when no spike is confirmed.
- Group stable siblings conservatively by parent identity and executable, with
  explicitly marked name-based fallback groups.
- Preserve unknown baselines, signed accounting residuals, partial scans, timing
  gaps and truncated history. Apply quality gates before labeling a primary
  observed CPU increase contributor.
- Reserve 48 MiB for history and 16 MiB for bounded attribution analysis within
  the 64 MiB recording/analysis budget; collector buffers remain separately bounded.
- Add deterministic analysis and attribution regressions. At that milestone, the macOS suite
  passed 6/6 tests and the Linux VM suite passed 7/7 with ASan/UBSan; Linux also
  built with `-Werror`.

### Existing prototype foundation

- Linux procfs process/CPU sampling with PID/start-time identities.
- Bounded immutable metadata snapshots and observed parent relationships.
- Bounded in-memory history and lifecycle observations, including visibility
  loss/restoration and disappearance intervals.
- `why sample` with optional details and lifecycle output, C17/CMake builds,
  portable fixtures and a Linux live lifecycle test.

### Still planned

Richer contextual explanations and performance benchmarking, including query
latency and recording overhead. See the [implementation guide](docs/implementation.md)
for current limits and validation, and the [usage guide](docs/usage.md) for available
commands. Rankings estimate observed contributions; they do not prove causality.
