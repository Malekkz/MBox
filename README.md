# MBox

MBox is a SCADA node that runs on one embedded Linux PC per site: it collects
and logs data, controls field devices, and serves a web HMI and a local CLI.

Status: design baseline frozen (version 1.0). Phase 0 (foundations) is
built: the build skeleton, its checks and the common library
(`libs/scada-common`: status and result types, logging, clocks). Phase 1 (data
plane) has started: the binary layout of the tag database, the tag-set
definition, the header builder, attach and tag read and write
(`libs/scada-tag`), and the daemon that creates the database and watches
liveness (`scada-tagd`).

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

Every test has one label: `unit`, `integration`, `perf`, `sanitizer` or
`fuzz`. `ctest` prints the time per label at the end of a run.

| Command | Runs |
| --- | --- |
| `ctest --preset dev` | Unit and integration tests |
| `ctest --preset unit` | Unit tests only (uses the `dev` build) |
| `ctest --preset integration` | Integration tests only (uses the `dev` build) |
| `ctest --preset perf` | Performance tests, in the `perf` build |
| `ctest --preset asan-ubsan` | Unit and integration tests under AddressSanitizer with leak detection and UndefinedBehaviorSanitizer |
| `ctest --preset tsan` | Unit and integration tests under ThreadSanitizer |
| `ctest --preset fuzz` | Every fuzz target for 60 s each; needs Clang |

Each preset has its own build directory, so configure and build it first, for
example `cmake --preset tsan && cmake --build --preset tsan`. The sanitizer
presets also run tests that prove each sanitizer catches a deliberate bug.
Change the fuzzing time with `-DSCADA_FUZZ_SECONDS=<seconds>` at configure
time. Where tests live and how to add one is in `tests/README.md`.

## Checking

```
cmake --build --preset dev --target check
```

Runs clang-format, clang-tidy, the banned-function check and the `SCADA_RT`
check over all C++ sources and fails on any finding. Needs Python 3.8 or newer
and clang-format, clang-tidy and clang-query 18 or newer. See
`tools/check/README.md`.

## Pipeline

`.github/workflows/pipeline.yml` runs on every pull request and on every push
to `main`. Each job is one check on the pull request, and each runs the
commands above and nothing else:

| Check | Runs | Machine |
| --- | --- | --- |
| `build and test (x86-64)` | `dev` and `release`: build, tests, staged install tree | x86-64 |
| `build and test (ARM64)` | The same, built and run natively | ARM64 |
| `check` | The `check` target | x86-64 |
| `sanitizers (asan-ubsan)` | Build and tests of the `asan-ubsan` preset | x86-64 |
| `sanitizers (tsan)` | Build and tests of the `tsan` preset | x86-64 |
| `fuzz` | Every fuzz target for 60 s | x86-64 |

A failing command fails its check: a compiler warning, a failing test, a
finding of the `check` target, a sanitizer report or a fuzzing crash. Nothing
is skipped or allowed to fail. Performance tests are not part of the pipeline
(`docs/decisions.md`, D-13), with one exception: both `build and test` jobs
run `scada-tag-wake-perf-test` on the `release` build and print the wake-up
latency of a change ring consumer, without gating on it, because the ARM64
runner is the only ARM64 machine of the project (D-31).

- The staged install tree of the `release` build is kept for 14 days as the
  artifacts `stage-x86-64` and `stage-ARM64` of the run.
- The licence report and the software bill of materials of the `release`
  build are kept for 14 days as the artifacts `dependency-reports-x86-64` and
  `dependency-reports-ARM64`. Both files are also printed in the job log, in
  the step "Licence report and bill of materials".
- When fuzzing fails, the failing inputs are kept as the artifact
  `fuzz-findings`.
- The tools are LLVM 18 and the GCC and CMake of the runner image; each job
  prints their versions in its step "Tool versions".
- All six checks must be marked as required in the protection rule of `main`.
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
