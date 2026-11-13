# MBox

MBox is a SCADA node that runs on one embedded Linux PC per site: it collects
and logs data, controls field devices, and serves a web HMI and a local CLI.

Status: design baseline frozen (version 1.0). Phase 0 (foundations) is in
progress; the code so far is the build skeleton, its checks and the common
library (`libs/scada-common`: status and result types, logging, clocks).

## Where things are

| Path | Content |
| --- | --- |
| `docs/reference-architecture.md` | The design baseline. Start here. |
| `docs/contracts/` | Interface contracts between the parts. |
| `docs/decisions.md` | Log of decisions taken during the build. |
| `docs/coding-standard.md` | C++ coding standard, focused on memory safety. |
| `libs/` | Libraries, one directory each (`lib<name>`). All logic lives here. |
| `services/` | Thin executables, one per service. |
| `tools/` | Developer and operator tools. |
| `tools/check/` | The `check` target: formatter, linter, banned-function and `SCADA_RT` checks. |
| `.clang-format`, `.clang-tidy` | Layout and lint rules. |
| `tests/` | Tests that span several components. |
| `cmake/` | Build helpers and toolchain files. |
| `docs/templates/component-README.md` | Template for a component's `README.md`. |
| `CLAUDE.md` | Rules for every change: code, tests, documentation, review. |
| `.github/issues-seed/` | The phase 0 and phase 1 issues, ready to create. |
| `scripts/create-issues.sh` | Creates those issues on GitHub. |

## Building

Needs CMake 3.21 or newer and GCC or Clang with C++20 support.

```
cmake --preset dev          # configure
cmake --build --preset dev  # build
ctest --preset dev          # test
```

Other presets: `release` (optimised, with the release flags of the coding
standard, section 2) and `perf` (the same flags with debug information, runs
performance tests only). Configuration stops if the compiler does not support
a required flag.

ARM64: on an ARM64 machine the same presets build natively. To cross-compile,
put a GNU cross toolchain (`aarch64-linux-gnu-g++`) on `PATH` and use the
presets `dev-arm64` or `release-arm64`. Tests of a cross build run if an
emulator is given at configure time with
`-DCMAKE_CROSSCOMPILING_EMULATOR="<emulator>;<arguments>"`.

## Checking

```
cmake --build --preset dev --target check
```

Runs clang-format, clang-tidy, the banned-function check and the `SCADA_RT`
check over all C++ sources and fails on any finding. Needs Python 3.8 or newer
and clang-format, clang-tidy and clang-query 18 or newer. See
`tools/check/README.md`.

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
