# Contributing

`why` is an early Linux process/CPU recorder prototype. Feedback, reproducible bug
reports, and design discussion are welcome. A license has not been selected yet;
external code contributions will open after the maintainer adds a `LICENSE` file.

## Build and test

Use a C17 compiler, CMake 3.16 or newer, and Make or Ninja. From the repository root:

```sh
mkdir -p build/tmp
export TMPDIR="$PWD/build/tmp"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DWHY_SANITIZE=ON
cmake --build build
(cd build && ctest --output-on-failure)
```

`WHY_SANITIZE` enables AddressSanitizer and UndefinedBehaviorSanitizer with GCC or
Clang. The default build does not enable sanitizers. Tests use explicit checks
that remain active in Release builds.

macOS runs the parser, calculation, metadata, ownership, history, spike detection
and attribution fixtures, IPC regressions, and CLI argument checks.
Linux additionally runs live lifecycle and recorder/query tests. They need readable
procfs and basic POSIX shell utilities. No tests require root. Fixtures create and
remove their files in the build working directory. IPC tests need permission to
bind local Unix sockets; a sandbox denial is an environment restriction, not a
passing test. Keep test runtime directories under the build directory, and do not
point fixtures at a live recorder.

The CI matrix targets Linux GCC, Linux Clang, and macOS Clang. Distinguish a local
successful test run from a hosted CI result that has not yet been observed.

## Performance measurements

Use the opt-in tools and Release build described in [docs/benchmarks.md](docs/benchmarks.md).
Keep timing runs separate from sanitizer correctness checks. Record process count,
workload, compiler/build flags, environment and sample counts alongside results.
Do not infer real collection performance from accelerated synthetic history.
Benchmark JSON should contain aggregate metrics only; keep local raw results under
an ignored build directory. Timing percentiles are observations, not CI thresholds.

## Style and ownership

Follow the LLVM-based `.clang-format`: indentation and tab width are four, with
`UseTab: Always`. Format edited C files; older untouched files may still use spaces.

```sh
clang-format -i src/your_changed_file.c
clang-format --dry-run --Werror src/your_changed_file.c
```

Keep compiler warnings clean on GCC and Clang. Use `why_` / `Why` prefixes for
externally visible symbols and `static` for private helpers. Prefer small functions,
explicit types, and clear error paths over unnecessary abstraction.

Keep procfs I/O separate from CPU arithmetic, history ownership, and presentation.
Document ownership in headers. Metadata is immutable and reference counted;
history views are borrowed until the next append or eviction. Append takes a
caller-owned frame that must not alias retained history. History's metadata ledger
counts its own references independently of the allocation's lifetime reference
count; keep both correct on eviction and failed append. Attribution results
own their row/index arrays but borrow process context: destroy the result before
mutating or releasing its history. Release allocations on every error path, and
do not change parser outputs when parsing fails.

Treat process data as untrusted input. Check bounds, identities, counter ranges,
read failures, and output escaping. An unavailable observation must not silently
become zero. Never infer an exact exit or proven causality from polling alone.

## Reporting an issue

Include the command, expected behavior, actual behavior, OS/kernel and compiler
versions, and a minimal reproduction. For CPU or history issues, include sampling
quality information if available. Prefer a synthetic fixture over a private
process dump. Review logs for credentials and sensitive arguments before sharing,
especially when `--details` was used.

## Preparing a change

Keep changes focused on one problem. Discuss CPU semantics, new dependencies, and
scope expansion before implementation. See the [implementation guide](docs/implementation.md)
for module boundaries and the [v0.1 specification](docs/v0.1-spec.md) for target behavior.

Add deterministic fixtures for changes to parsing, ownership, counters, or temporal
logic. Live tests supplement those fixtures; they do not replace them. If a change
alters measurement semantics or a design threshold, update the relevant docs and
acceptance cases as well.

A pull request should explain the problem, resulting behavior, validation, and
remaining limits. Do not claim that Linux behavior, hosted CI, or performance
has been validated unless those checks were actually run.

Keep user-visible behavior in `docs/usage.md`, module contracts and validation in
`docs/implementation.md`, and milestone changes in `CHANGELOG.md`. Update the
README status when a planned command becomes available. The detailed specification
is a design target; distinguish implemented behavior from remaining work.
