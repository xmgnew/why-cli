# Changelog

Development checkpoints are recorded here. The project has not published a
completed v0.1 release; the CMake version `0.1.0` is a development version.

## Unreleased

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
- Add deterministic analysis and attribution regressions. The latest macOS suite
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

Foreground recording with cross-terminal queries, richer contextual explanations
and performance benchmarking. See the [implementation guide](docs/implementation.md)
for current limits and validation, and the [usage guide](docs/usage.md) for available
commands. Rankings estimate observed contributions; they do not prove causality.
