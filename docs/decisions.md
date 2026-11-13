# Decisions log

Every choice the reference document left open is recorded here, newest last.
An entry is added in the same pull request that acts on the decision.

Decisions already made in the reference document are not repeated here; see its
section 2 (decision summary) and section 19 (open items).

## Format

```
## D-<number>: <short title>
Date: YYYY-MM-DD
Phase / issue: <phase>, #<issue>
Status: accepted | superseded by D-<n>

Context: what had to be decided, and why now.
Options: the alternatives considered, one line each.
Decision: what was chosen.
Reason: why, in one or two sentences.
Consequences: what this fixes or rules out later.
```

## Open decisions carried from the reference document

| Item | Needed by |
| --- | --- |
| Licensing policy for third-party protocol stacks | Start of phase 9 |
| Linux distribution and package format | Start of phase 10 |
| Minimum hardware for the full 50,000 tags | After the phase 3 benchmark |
| Control logic model beyond function blocks and the structured-text subset | After phase 6 |

## Decisions

## D-1: C++ coding standard and memory-safety gates
Date: 2026-11-13
Phase / issue: phase 0, before issue #3
Status: accepted

Context: the rules in `CLAUDE.md` named no baseline and covered memory safety
only briefly, while phase 0 was about to fix compiler flags and linter checks.
Options: C++ Core Guidelines; MISRA C++:2023; a project-only rule list.
Decision: `docs/coding-standard.md`, based on the C++ Core Guidelines with
stricter project rules for ownership, bounds, integers, shared memory and
parsing. Sanitizer runs gate every pull request, and every parser ships with a
fuzz target.
Reason: the Core Guidelines are free to use and are checked directly by
clang-tidy; MISRA needs a paid document and commercial checkers.
Consequences: fuzz targets are written with each parser from phase 2 onward,
earlier than the phase 10 fuzzing item in the reference document, which
becomes the long-running fuzzing campaign.

## D-2: CMake 3.21 as the minimum version, presets for every build
Date: 2026-11-13
Phase / issue: 0, #2
Status: accepted

Context: the build needs named configurations (`dev`, `release`, `perf`) that give the same result on every machine and in the pipeline.
Options: CMake presets (needs CMake 3.21 or newer); wrapper scripts; documented command lines.
Decision: `CMakePresets.json` version 3, and `cmake_minimum_required(VERSION 3.21)`. Presets name no generator; CMake picks the platform default.
Reason: presets are part of CMake, so no extra tool or script is needed, and 3.21 is the oldest version that supports the preset features used.
Consequences: build directories are `build/<preset>`. A machine with an older CMake cannot build the project.

## D-3: How the compiler settings are applied, and the `perf` preset
Date: 2026-11-13
Phase / issue: 0, #2
Status: accepted

Context: section 2 of the coding standard lists the flags, but not how the build applies them, and the issue asks for a `perf` preset without defining it.
Options: flags set globally or per target; an unsupported flag skipped or fatal; `perf` as the `release` build itself or a separate build.
Decision:
- The flags are attached per target by `scada_target_defaults`. The "all builds" list is always on; the option `SCADA_HARDENING` adds the release list.
- Configuration stops if the compiler rejects a required flag or cannot provide fortify level 3. Nothing is skipped automatically.
- `release` is `Release` with hardening, with CMake's default `-O3` replaced by the `-O2` of the standard. `perf` is `RelWithDebInfo` (`-O2 -g`) with hardening and runs only tests labelled `perf`.
- `-fPIE -pie` are produced by CMake's position-independent-code support instead of being written as literal flags.
- `-U_FORTIFY_SOURCE` precedes `-D_FORTIFY_SOURCE=3`, because some toolchains predefine the macro and the redefinition would be a warning.
Reason: per-target flags keep our warning policy off third-party code added later. A fatal error is the only way a flag cannot be dropped silently. Performance must be measured with the hardening that ships, and debug information makes a regression diagnosable.
Consequences: `dev` builds have the warnings and library assertions but not the release flags. Performance figures come from the `perf` preset only. No substitute was needed: GCC 13.3 (x86-64 and ARM64 cross) and Clang 18.1 accept every flag and provide fortify level 3.

Known limit, ARM64: `-mbranch-protection=standard` is applied and our objects carry the BTI and PAC markings, but the linked executable is marked BTI-enabled only if every linked object is, including the toolchain's start files. With the cross toolchain used here they are not, so return-address signing is active and branch-target enforcement is not. This depends on the toolchain that builds the release, which is chosen with the distribution (reference 19.1); it must be checked again then.

## D-4: ARM64 is built by cross-compilation with a GNU toolchain file, or natively
Date: 2026-11-13
Phase / issue: 0, #2
Status: accepted

Context: the same source must build for ARM64 (reference 4.1). How the pipeline runs ARM64 is decided in issue P0-04.
Options: native build only; a cross toolchain file; a container image per architecture.
Decision: `cmake/toolchains/aarch64-linux-gnu.cmake` and the presets `dev-arm64` and `release-arm64` cross-compile with the compiler `aarch64-linux-gnu-g++` found on `PATH`; the prefix can be changed with `SCADA_CROSS_PREFIX`. On an ARM64 machine the normal presets build natively.
Reason: the triplet prefix is the upstream GNU naming, so no distribution path is needed, and both ways of building stay available to P0-04.
Consequences: tests of a cross build run only when an emulator is given with `CMAKE_CROSSCOMPILING_EMULATOR` at configure time.

## D-5: Libraries are static; target naming
Date: 2026-11-13
Phase / issue: 0, #2
Status: accepted

Context: each service is a library plus a thin executable with the same name (`libscada-hello`, `scada-hello`), and CMake target names must be unique.
Options: static or shared libraries; target names `lib<name>` or a suffix on the executable.
Decision: libraries are static, created with `scada_add_library(<name>)` as target `lib<name>` (file `lib<name>.a`) with the alias `scada::<short name>`. Executables are created with `scada_add_executable(<name>)`.
Reason: static linking keeps each service one file to install and keeps a combined build possible; the alias gives dependants a stable name.
Consequences: libraries are not installed. Driver plugins, if the driver interface contract chooses plugins, will need a shared-library helper.

## D-6: Dependency list and its check are plain JSON and CMake
Date: 2026-11-13
Phase / issue: 0, #8
Status: accepted

Context: reference 16.3 requires a build that fails on a dependency whose licence is not approved, and issue #8 requires it to fail on an unlisted dependency. Neither says where the list lives or how a dependency is detected.
Options: a package manager manifest (vcpkg, Conan); a scanner tool run after the build; a JSON list checked by a CMake module at configure time.
Decision: `third-party/dependencies.json` is the list and `third-party/licence-allowlist.json` the allow-list. `cmake/ScadaDependencies.cmake` validates both at configure time, then checks every link item of every target of this project: it must be a target of this project, a `link_names` value of a list entry, or a platform library. A directory under `third-party/` must be named after an entry. The two files are configure dependencies, so a build after an edit configures again.
Reason: it needs no tool beyond CMake, which the build already requires, and no package manager, which the reference forbids relying on.
Consequences: every dependency must be linked by a name, including header-only ones (an `INTERFACE` target). Not detected: headers used through an include path alone, and what vendored or fetched third-party code links itself; review covers these. Link items written as generator expressions other than plain wrappers are rejected.

## D-7: What counts as a dependency, and the first allow-list
Date: 2026-11-13
Phase / issue: 0, #8
Status: accepted

Context: the licensing policy is open until phase 9 (reference 16), but the allow-list must exist now.
Options: allow-list of permissive licences only; also weak-copyleft licences (LGPL, MPL, EPL); licence expressions or one identifier per entry.
Decision:
- The allow-list holds Apache-2.0, BSD-2-Clause, BSD-3-Clause, BSL-1.0, CC0-1.0, ISC, MIT, Zlib and blessing (the SQLite dedication). No GPL, LGPL, MPL or EPL.
- `licence` is one SPDX identifier, compared exactly. A dependency offered under several licences is listed with the one it is used under.
- The compiler, its runtime libraries and the C library (including `pthread`, `m`, `dl`, `rt`, `atomic`) belong to the toolchain, are chosen with the distribution (reference 19.1) and are not listed.
- A change to the allow-list needs review by a person and a decision entry.
Reason: permissive-only is the one set that every policy option of reference 16.2 accepts, so nothing adopted now can conflict with the later decision. Exact matching of one identifier cannot be bypassed by an expression such as `MIT OR GPL-3.0-only`.
Consequences: libmodbus (LGPL), open62541 (MPL) and Eclipse Tahu (EPL) cannot be added until the licensing decision extends the allow-list. The bill of materials does not cover the toolchain runtime or the operating system; that is added with packaging in phase 10.

## D-8: Bill of materials format and where the reports go
Date: 2026-11-13
Phase / issue: 0, #8
Status: accepted

Context: reference 13.6 requires a software bill of materials and 16.3 a licence report, without naming formats or locations.
Options: CycloneDX JSON; SPDX JSON; a project-specific format.
Decision: the build writes `build/<preset>/reports/sbom.cdx.json` (CycloneDX 1.5 JSON) and `build/<preset>/reports/licence-report.md`, and installs both to `share/doc/scada/` of the install tree. Dependencies with scope `build` or `test` are in the bill of materials with CycloneDX scope `excluded`. The file has no timestamp and no serial number.
Reason: CycloneDX needs no generated identifiers or timestamps, so the same input gives the same file, and common scanners read it.
Consequences: the pipeline (issue P0-04) uploads `build/<preset>/reports/` as an artifact. The generated file is checked by tests for content, not against the CycloneDX schema, because no validator is available offline.
