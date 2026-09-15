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
and attribution fixtures, plus CLI argument checks.
Linux additionally runs a live process-lifecycle test. That test needs readable
procfs and basic POSIX shell utilities. No tests require root. Fixtures create and
remove their files in the build working directory.

The CI matrix targets Linux GCC, Linux Clang, and macOS Clang. Distinguish a local
successful test run from a hosted CI result that has not yet been observed.

## Style and ownership

Use the LLVM-based `.clang-format`: four spaces and no tabs.

```sh
clang-format -i src/*.c src/*.h tests/*.c
clang-format --dry-run --Werror src/*.c src/*.h tests/*.c
```

Keep compiler warnings clean on GCC and Clang. Use `why_` / `Why` prefixes for
externally visible symbols and `static` for private helpers. Prefer small functions,
explicit types, and clear error paths over unnecessary abstraction.

Keep procfs I/O separate from CPU arithmetic, history ownership, and presentation.
Document ownership in headers. Metadata is immutable and reference counted;
history views are borrowed until the next append or eviction. Attribution results
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
