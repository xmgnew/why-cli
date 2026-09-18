# Changelog

Development checkpoints are recorded here. The project has not published a
completed v0.1 release; the CMake version `0.1.0` is a development version.

## Unreleased

### History storage efficiency

- Charge each immutable metadata allocation once per history, including shared
  parent references; include the bounded accounting index in the same quota.
- Allocate lifecycle arrays for observed events only. Preserve full process
  snapshots, unknown values, identity tracking and CPU calculations.
- Add accounting breakdowns and regression coverage for shared ownership,
  metadata replacement, eviction and failed append rollback.
- Extend the 1000-process synthetic retained span from 37 to 119 seconds within
  the unchanged 48 MiB history quota. Five-minute retention is not yet achieved
  for this scenario; retaining more frames can increase RSS and query work.

### Performance baseline

- Add opt-in `why_bench` measurements for synthetic retention, live collection and
  report rendering, plus a Linux end-to-end query tool with controlled workloads.
- Report aggregate timing percentiles, CPU, RSS, history coverage and scan quality
  without saving process details or changing normal recorder behavior.
- Establish a Release baseline and document the 37-second retained span in the
  synthetic 1000-process metadata scenario; do not claim the live-process target
  is met. Keep sanitizer correctness checks separate from timing runs.

### Context evidence

- Show up to eight ancestor levels from the contributor's original retained
  snapshot, stopping on missing identities, cycles or the depth limit.
- Add conservative start-near-rise and disappearance-near-recovery evidence;
  keep tick/clock uncertainty, gaps, history expiry and unknown values explicit.
- Label group context as a representative member and avoid group-wide lifecycle
  claims. Context never changes CPU totals, increment rankings or quality gates.
- Add context fixtures and escaped-output regressions; increase the bounded IPC
  response buffer to 128 KiB for ancestor output.

### Runtime lifecycle

- Switch Linux recording/query IPC to abstract Unix sockets: no runtime directory,
  socket file or lock file is created. Closing the endpoint, including after
  forced termination, permits restart without stale-file cleanup.
- Preserve same-UID checks and atomic duplicate prevention. `WHY_SOCKET_NAME`
  selects an independent session; Linux ignores the old `WHY_RUNTIME_DIR` setting.
- Leave old-version artifacts untouched and document migration. macOS live
  recording remains unsupported; its build-local IPC fixtures are unchanged.
- Add Linux forced-termination/rebind and no-runtime-file regression checks.

### Foreground recorder and queries (previous checkpoint)

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

Further process snapshot compaction, broader real-workload performance validation and
explicit opt-in query metadata expansion. See the [implementation guide](docs/implementation.md)
for current limits and validation, and the [usage guide](docs/usage.md) for available
commands. Rankings estimate observed contributions; they do not prove causality.
