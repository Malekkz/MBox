# MBox

MBox is a SCADA node that runs on one embedded Linux PC per site: it collects
and logs data, controls field devices, and serves a web HMI and a local CLI.

Status: design baseline frozen (version 1.0). Phase 0 (foundations) is
built: the build skeleton, its checks and the common library
(`libs/scada-common`: status and result types, logging, clocks). Phase 1 (data
plane) has started: the binary layout of the tag database, the tag-set
definition, the header builder, attach and tag read and write
(`libs/scada-tag`), the daemon that creates the database and watches
liveness (`scada-tagd`), and the simulator writer (`scada-sim`).

## Where things are

| Path | Content |
| --- | --- |
| `docs/reference-architecture.md` | The design baseline. Start here. |
| `docs/contracts/` | Interface contracts between the parts. |
| `docs/decisions.md` | Log of decisions taken during the build. |
| `docs/coding-standard.md` | C++ coding standard, focused on memory safety. |
| `docs/install.md` | Installing the staged tree on a test machine by hand. |
| `libs/` | Libraries, one directory each (`lib<name>`). All logic lives here. |
| `services/` | Thin executables, one per service. |
| `tools/` | Developer and operator tools. |
| `tools/check/` | The `check` target: formatter, linter, banned-function and `SCADA_RT` checks. |
| `.clang-format`, `.clang-tidy` | Layout and lint rules. |
| `.github/workflows/pipeline.yml` | The pipeline that gates every pull request. |
| `tests/` | Tests that span several components, and the test layout. |
| `cmake/` | Build helpers and toolchain files. |
| `third-party/` | Dependency list, licence allow-list, vendored code. |
| `deploy/` | Templates for systemd units and service users, directories created at boot, and the parts of the install tree that belong to no component. |
| `docs/templates/component-README.md` | Template for a component's `README.md`. |
| `CLAUDE.md` | Rules for every change: code, tests, documentation, review. |
| `.github/issues-seed/` | The phase 0 and phase 1 issues, ready to create. |
| `scripts/create-issues.sh` | Creates those issues on GitHub. |
| `scripts/measure-seqlock.sh` | Measures the read retry window and the rate of `SCADA_E_BUSY` on this machine (decision D-31). |
| `scripts/run-ctest-test.sh` | Runs one CTest test directly, without its time limit, for the 10-minute runs (decision D-57). |
| `docs/reports/` | Phase reports: the figures a phase is accepted on. |

## Building

Needs CMake 3.21 or newer and GCC or Clang with C++20 support. The first
configuration of a build directory downloads GoogleTest 1.17.0 (checked
against a fixed hash); to build without network access, pass
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<unpacked release>`.

```
cmake --preset dev          # configure
cmake --build --preset dev  # build
ctest --preset dev          # unit and integration tests
```

Other presets: `release` (optimised, with the release flags of the coding
standard, section 2) and `perf` (the same flags with debug information, runs
performance tests only). Configuration stops if the compiler does not support
a required flag.

Every build checks the third-party dependencies against
`third-party/dependencies.json` and the licence allow-list, and stops if one
is unlisted or its licence is not allowed. It writes the licence report and
the software bill of materials to `build/<preset>/reports/`, and the staged
install tree carries both in `usr/share/doc/scada/`. To add a
dependency, see `third-party/README.md`.

The tests need `systemd-analyze` from systemd 250 or newer, to check the
generated service units.

Staged install tree: `cmake --install build/dev --prefix stage` writes an
image of the paths the software owns on the target (`usr/bin`, `etc/scada`,
`var/lib/scada` and so on) under `stage/`. See `docs/install.md`.

ARM64: on an ARM64 machine the same presets build natively. To cross-compile,
put a GNU cross toolchain (`aarch64-linux-gnu-g++`) on `PATH` and use the
presets `dev-arm64` or `release-arm64`. Tests of a cross build run if an
emulator is given at configure time with
`-DCMAKE_CROSSCOMPILING_EMULATOR="<emulator>;<arguments>"`.

## Testing

Every test has one label: `unit`, `integration`, `perf`, `model`, `sanitizer`
or `fuzz`. `ctest` prints the time per label at the end of a run.

| Command | Runs |
| --- | --- |
| `ctest --preset dev` | Unit and integration tests (not `perf` or `model`) |
| `ctest --preset unit` | Unit tests only (uses the `dev` build) |
| `ctest --preset integration` | Integration tests only (uses the `dev` build) |
| `sudo ctest --preset root` | Tests that must run as root to become other users (label `root`, uses the `dev` build); the other presets leave them out. Run by hand, it leaves `build/dev/Testing` owned by root, so that a later unprivileged `ctest --preset dev` cannot write its log there: `sudo chown -R "$USER" build/dev/Testing` afterwards. The pipeline runs it on GitHub-hosted runners only, which are discarded after the job; a self-hosted runner must not run it |
| `ctest --preset perf` | Performance tests, in the `perf` build |
| `ctest --preset model` | Model check of the sequence lock, the change ring and the repair after an owner restart (conformance test 4); needs herd7, see `tests/model/README.md`. Only this preset runs it. |
| `ctest --preset asan-ubsan` | Unit and integration tests under AddressSanitizer with leak detection and UndefinedBehaviorSanitizer |
| `ctest --preset tsan` | Unit and integration tests under ThreadSanitizer |
| `ctest --preset fuzz` | Every fuzz target for 60 s each; needs Clang |

Each preset has its own build directory, so configure and build it first, for
example `cmake --preset tsan && cmake --build --preset tsan`. The sanitizer
presets also run tests that prove each sanitizer catches a deliberate bug.
Change the fuzzing time with `-DSCADA_FUZZ_SECONDS=<seconds>` at configure
time. The `check` target is not a test; how its linter selects files on a
pull request is described under "Checking" below, and its self-test is
`ctest --preset dev -R scada-check-selection-test`. The pipeline splits a
full clang-tidy lint across four jobs; the self-test of that split, on its
own lists and on the translation units of the source tree, is
`ctest --preset dev -R scada-check-shares-test` (see "Pipeline"). Where
tests live and how to add one is in `tests/README.md`.

## Checking

```
cmake --build --preset dev --target check
```

Runs clang-format, clang-tidy, the banned-function check and the `SCADA_RT`
check over all C++ sources and fails on any finding. Needs Python 3.8 or newer
and clang-format, clang-tidy and clang-query 18 or newer. See
`tools/check/README.md`.

How clang-tidy selects files: by default it lints every translation unit.
Configured with `-DSCADA_CHECK_TIDY_BASE=<revision>`, it lints only the
units that changed since the merge base of that revision and `HEAD`, and the
units that include a changed file, directly or through other headers, as
the compiler's dependency output (`-M`) reports it. It lints everything when
`.clang-tidy`, a CMake file or preset, a toolchain file, the pipeline, the
setup action, `tools/check/`, `third-party/` or a `*.in` file changed, when a
header was deleted or renamed, and when the changed set cannot be worked out
(no merge base, a shallow clone, an error). The first line of its output
names the mode, the reason and the number of units. The pipeline sets the
option on a pull request; a push to `main` lints everything. The formatter,
banned-function and `SCADA_RT` checks always run on every file. Clear the
option with `-DSCADA_CHECK_TIDY_BASE=` (decision D-67).

The target is one run of all four checks, also when clang-tidy lints
everything. The pipeline runs it in parts with three further options, which
are empty by default: `SCADA_CHECK_ONLY` (a list of `format`, `tidy`,
`banned`, `rt`), `SCADA_CHECK_TIDY_SHARE` (`K/N`: lint only share K of N of
the selected units) and `SCADA_CHECK_TIDY_RECORD` (a file the share writes
what it linted to). See `tools/check/README.md`, "Splitting the lint across
jobs" (decision D-68).

## Pipeline

`.github/workflows/pipeline.yml` runs on every pull request and on every push
to `main`. Each job is one check on the pull request, and each runs the
commands above and nothing else:

| Check | Runs | Machine |
| --- | --- | --- |
| `build and test (x86-64)` | `dev` and `release`: build, tests, staged install tree; the `root` tests of `dev` with sudo, conformance test 26 of `release` with sudo; `perf`: the performance gate | x86-64 |
| `build and test (ARM64)` | The same, built and run natively | ARM64 |
| `check` | Passes only if `check (format, banned, rt)` and every `check (tidy K of 4)` passed and the four shares together linted every selected translation unit exactly once; prints the totals | x86-64 |
| `sanitizers (asan-ubsan)` | Build and tests of the `asan-ubsan` preset | x86-64 |
| `sanitizers (tsan)` | Build and tests of the `tsan` preset | x86-64 |
| `fuzz` | Every fuzz target for 60 s | x86-64 |
| `model check` | herd7 builds; `ctest --preset model` (conformance test 4) | x86-64 |

The `check` target runs in five further jobs, which `check` sums up
(decision D-68). `check (format, banned, rt)` runs the formatter, the
banned-function and the `SCADA_RT` checks on every file, once, and the
self-tests of the lint selection and of the split. `check (tidy 1 of 4)` to
`check (tidy 4 of 4)` each configure the `dev` build, work out the same
selection of translation units (on a pull request only the units it could
have affected, see "Checking"), and lint their share of it: a round-robin
over the list sorted by path. A selection of fewer than 40 units is not
split; share 1 lints all of it. Each share prints its number and its units,
and keeps a record of them for `check`, which fails if a share failed, timed
out, was cancelled or skipped, if a record is missing, or if a unit was
linted twice or not at all. A full lint of 165 units takes about 11 minutes
of wall time instead of 38.

A failing command fails its check: a compiler warning, a failing test, a
finding of the `check` target, a sanitizer report or a fuzzing crash. Nothing
is skipped or allowed to fail. The step "Perf gate (perf)" of both `build
and test` jobs runs `ctest --preset perf` (decision D-57, which replaces the
line of D-13 that kept performance tests out of the pipeline). It fails on a
target of reference 15.2 or contract 16 on either architecture: the
simulator rate of 50,000 updates per second (D-41), a wake-up 99th
percentile below 1 ms (D-32), the scan of 5,000 tags below 0.5 ms, the
database below 64 MB, all phase 1 processes below 1.5 GB, a queue post
below 10 us per record; and on a median of conformance test 27 more than
2.0 times its baseline in `tests/perf/baseline/<processor>.txt`, for the
calls the baseline names. The runners are shared virtual machines: the
tolerance catches a regression of a multiple, not of 10 %.

- The staged install tree of the `release` build is kept for 14 days as the
  artifacts `stage-x86-64` and `stage-ARM64` of the run.
- The licence report and the software bill of materials of the `release`
  build are kept for 14 days as the artifacts `dependency-reports-x86-64` and
  `dependency-reports-ARM64`. Both files are also printed in the job log, in
  the step "Licence report and bill of materials".
- When fuzzing fails, the failing inputs are kept as the artifact
  `fuzz-findings`.
- The tools are LLVM 18 and the GCC and CMake of the runner image; each job
  prints their versions in its step "Tool versions". `model check` also
  builds herd7 from source, at the version and commit listed in
  `third-party/dependencies.json`, and keeps the build in the Actions cache
  under that commit and the runner image.
- The workflow `measurements` (`.github/workflows/measurements.yml`) is not
  a check. Started by hand, on x86-64 and on ARM64 with `isb` and with
  `yield`, and every Monday at 03:17 UTC on x86-64 and ARM64 with `isb`, it
  runs in the `perf` build `scripts/measure-seqlock.sh`, conformance tests 2
  and 26 and the ring-gap test for 600 s each, the huge-page reads as root,
  and every `perf` test, and keeps the output for 90 days as the artifacts
  `measurements-<arch>-<pause>` (decisions D-31, D-57, D-64). Started by hand with `latency_spread`, it runs conformance test 27
  on 10 fresh runners per architecture as well as the measurement jobs. The weekly run costs
  Actions minutes; to stop it, delete its `schedule` or disable the
  workflow on the Actions page.
- `build and test (ARM64)` also checks, in `ctest --preset release`, that the
  ARM64 tag library compiles to the order of accesses the model check
  describes, and that edited copies of the sources, compiled with the
  runner's GCC, fail that check (`tests/model/README.md`).
- All seven checks of the table must be marked as required in the protection
  rule of `main`. The jobs `check (format, banned, rt)` and `check (tidy K of
  4)` need not be: `check` fails when any of them does not pass.
  GitHub cannot read this from the repository; it is a repository setting.

## Build order

The build follows the phases in section 18 of the reference document. Phases 0
to 2 are built one issue at a time, because everything else depends on them.
From phase 3, parts that depend only on frozen contracts can be built in
parallel.

## First steps

1. Review and merge the pull request that adds this starting set.
2. Protect the `main` branch: require passing checks and one review.
3. Start with issue #2 (P0-01).

The phase 0 and 1 issues (#2 to #21) already exist on GitHub. Do not run
`scripts/create-issues.sh` on this repository again, or they will be
duplicated; the script is kept for creating later phases' issues the same way.
