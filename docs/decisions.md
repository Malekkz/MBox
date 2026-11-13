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

## D-6: The `check` target is a script around clang-format, clang-tidy and clang-query
Date: 2026-11-13
Phase / issue: 0, #3
Status: accepted

Context: the coding standard (section 13) names four checks in the `check` target. clang-format and clang-tidy exist; the banned-function check and the `SCADA_RT` check had to be built.
Options: for the two new checks, a text search; a clang-tidy plugin in C++; clang-query matchers on the syntax tree. For the driver, CMake script or Python.
Decision: `tools/check/scada_check.py` runs all four checks and fails on any finding. The two new checks are clang-query matchers held in that script, reported as `scada-banned` and `scada-rt`. The target needs Python 3.8 or newer and clang-format, clang-tidy and clang-query 18 or newer; the script refuses older versions.
Reason: a text search cannot tell a call from a comment, a string or a member of the same name, and cannot see which function a call is in. A plugin must be built against one LLVM version. Matchers need no build and work on the real syntax tree.
Consequences:
- The tools are needed only for `check`. A configuration without them warns, and the target fails and names what is missing.
- A source file that the build does not compile, or that does not compile with Clang, fails the check; it is never skipped.
- Different clang-format versions can lay out the same code differently. The pipeline (P0-04) must fix one version.
- LLVM (Apache-2.0 with LLVM exception) and Python (PSF licence) are tools of the build machine and are not linked or shipped. The licence report (P0-07) must list them.
- Headers are checked through the files that include them. A header that nothing includes is formatted but not analysed.

## D-7: clang-tidy configuration: errors, disabled checks, naming details
Date: 2026-11-13
Phase / issue: 0, #3
Status: accepted

Context: the coding standard (section 13) enables nine check groups and makes five of them errors, while the same section and issue #3 say that any finding fails the `check` target. `CLAUDE.md` gives the naming rules in one line.
Options: make all groups errors in `.clang-tidy`; or keep the five there and let the target fail on everything.
Decision:
- `.clang-tidy` lists the five error groups as the standard does, so editors show the rest as warnings. The `check` target runs clang-tidy with every finding as an error.
- Disabled as unworkable: `bugprone-easily-swappable-parameters` (reports every two adjacent parameters of one type, such as two streams; the only fix is a wrapper type per parameter) and `modernize-use-trailing-return-type` (asks for `auto f() -> T` on every function; the project writes the return type first).
- Naming, where `CLAUDE.md` is not specific: namespaces are `snake_case`. "Constants" are constants at namespace, class or static scope, every `constexpr` variable, enumerators and macros. A `const` local variable or parameter is a variable and is `snake_case`. Private data members end with `_`. Template type parameters are `PascalCase`.
Reason: one finding list for the gate, without changing what the standard calls an error. The naming details follow the code already merged for #2.
Consequences: a check disabled later is added to this list in the same pull request.

## D-8: How the banned list is enforced
Date: 2026-11-13
Phase / issue: 0, #3
Status: accepted

Context: some rows of the banned list (coding standard, section 12) depend on things a tool cannot see: "`strlen` on outside data", "`strtol` without full error checks", "outside reviewed helpers".
Options: leave those rows to review; or ban the functions everywhere and suppress the reviewed uses.
Decision: the check reports every use of every function and construct in the list, including `strncpy`, `strlen`, the `atoi` and `strtol` families, `memcpy`, `memmove`, `memset` and `reinterpret_cast`. A reviewed helper suppresses its own line with `// NOLINT(scada-banned): reason` (section 14). A `NOLINT` that does not name the check is not honoured. A cast to `void` to discard a value is not reported as a C-style cast.
Reason: a rule a tool cannot decide is not a gate. The suppressions make the reviewed uses visible and countable.
Consequences: the helpers of `libscada-common` and `libscada-tag` will carry the first suppressions, each listed in its pull request. Taking the address of a banned function is reported like a call.

## D-9: `SCADA_RT` and the first real-time check
Date: 2026-11-13
Phase / issue: 0, #3
Status: accepted

Context: `CLAUDE.md` requires real-time functions to be marked with `SCADA_RT` so that checks can find them. Neither the form of the annotation nor its header was specified.
Options: a comment marker; a naming convention; an attribute.
Decision:
- `SCADA_RT` is defined in `scada/common/realtime.h` of `libscada-common`. Where the compiler has the `clang::annotate` attribute it expands to `[[clang::annotate("scada_rt")]]`, otherwise to nothing. It is written first in the declaration.
- The check reports, inside a marked function or a lambda in it: `new`, `delete`, the C allocation functions, `std::make_unique`, `std::make_shared`, `std::allocate_shared`, the `printf` family and other C stream output, locking a standard mutex, constructing a standard lock object, and `pthread_mutex_lock`.
- `libscada-common` is a header-only (interface) target until P0-06 adds its first sources.
Reason: an attribute is part of the syntax tree, so the check finds the function without guessing, and a declaration in a header marks the definition too. The standard attribute syntax with a vendor namespace is not a language extension; the macro keeps GCC, which warns about unknown attributes, free of it.
Consequences:
- Only direct calls are found. A marked function that calls an unmarked function which allocates is not reported; real-time code is still reviewed against section 11.
- Project code must not use the `annotate` attribute for anything else.
- Blocking calls, system calls and C++ stream output are not on the first deny-list. They are added as the real-time code appears (phase 1, `libscada-tag`).

## D-10: GoogleTest as the unit test framework
Date: 2026-11-13
Phase / issue: 0, #4
Status: accepted

Context: the tests need one framework before any real code is written; its licence must be permissive.
Options: GoogleTest 1.17.0 (BSD-3-Clause); Catch2 3 (BSL-1.0); doctest (MIT); no framework.
Decision: GoogleTest 1.17.0, without GoogleMock. `cmake/ScadaTesting.cmake` downloads the release archive at configure time, checks its SHA-256 hash and builds it inside the build tree. `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<directory>` builds from an unpacked copy without network access. Its headers are treated as system headers.
Reason: it is the most widely used C++ framework, so reviewers and tools know it, and it has death tests and per-test timing that the later phases need. It must be built from source in the tree because a sanitizer needs every object in the process instrumented, and a cross build needs it for the target architecture; a copy installed on the machine gives neither.
Consequences:
- The first configuration of a build directory needs network access or the override above.
- GoogleTest is test-only and is not installed or shipped. The licence report (P0-07) must list it: GoogleTest 1.17.0, BSD-3-Clause, test-only.
- Its macro `GTEST_TEST_`, which `TEST` and `TEST_F` expand to, registers each test with `new`. The banned-function check (D-8) does not report that one expression; a `new` anywhere else in a test file is still reported. Without this, every test would need a suppression.
- After a fatal assertion such as `ASSERT_TRUE(value.has_value())`, clang-tidy still reports the access to the optional, because it cannot see through the macro. Tests write `if (!value.has_value()) { FAIL() << ...; }` instead, or compare the optional itself.

## D-11: Test kinds, labels and where tests live
Date: 2026-11-13
Phase / issue: 0, #4
Status: accepted

Context: `CLAUDE.md` names unit, integration and performance tests but not how the build tells them apart.
Options: one CTest test per test case or per executable; labels or separate build trees per kind.
Decision:
- `scada_add_test(<name> KIND unit|integration|perf ...)` creates one GoogleTest executable, registered as one CTest test with the kind as its label and a time limit of 60 s, 300 s or 600 s.
- Unit tests live in `<component>/tests/`. Integration tests live in `tests/integration/`, performance tests in `tests/perf/`, shared test code in `tests/support/`.
- `ctest --preset dev` runs unit and integration tests and prints the time per label. `ctest --preset unit` and `ctest --preset integration` run one kind. Performance tests are built everywhere but run only by `ctest --preset perf`.
- Integration tests start processes with `scada::test::ChildProcess`, which waits in `poll()` for output or exit with a timeout. The command to start is passed in an environment variable so that a cross build can put the emulator in front.
Reason: one CTest test per executable needs no test discovery at build time, which a cross build without an emulator cannot do. Performance figures from a debug build mean nothing, so no other preset runs them.
Consequences: CTest reports an executable, not a test case; the case that failed is in the test output, which the presets print on failure. `ChildProcess` uses `pidfd_open`, so the tests need Linux 5.3 or newer.

## D-12: Sanitizer and fuzz builds
Date: 2026-11-13
Phase / issue: 0, #4
Status: accepted

Context: section 13 of the coding standard requires the sanitizer runs and fuzz targets, but not how they are built.
Options: sanitizer flags per target or for the whole tree; fuzz targets built only in the fuzz build or in every build.
Decision:
- The cache variable `SCADA_SANITIZER` selects `address-undefined`, `thread` or `fuzz`; the presets `asan-ubsan`, `tsan` and `fuzz` set it. The flags apply to the whole build tree, including GoogleTest. Every finding ends the process with a failure (`-fno-sanitize-recover=all`). Configuration stops if the runtime libraries are missing, or if a sanitizer is combined with `SCADA_HARDENING`.
- `asan-ubsan` and `tsan` are debug builds with the default compiler. `fuzz` uses Clang (`clang++` on `PATH`), because libFuzzer is part of Clang, with `RelWithDebInfo`.
- `scada_add_fuzz_target(<name> ...)` creates a fuzz target with its seed corpus in `corpus/<name>/` next to it. In the `fuzz` build, the test `<name>` (label `fuzz`) runs it for `SCADA_FUZZ_SECONDS` (default 60 s) with a 10 s limit per input. In every other build the same target is linked with a driver that replays the seed corpus once, as the unit test `<name>-corpus`.
- A fuzz target lives in `<component>/fuzz/`. Inputs the fuzzer finds and the inputs that made it fail are written to the build tree, never to the source tree.
- Deliberate bugs that prove the tools work (`tests/sanitizer-fixtures/`, the toy parser in `tests/fuzz-sample/`) are test-only, are not installed, and their tests pass only if the tool reports the bug and the process fails. The fixtures are built only in the sanitizer builds, and the `check` target excludes them, as it excludes its own fixtures; the toy parser is checked, with one suppression on the planted bug.
- A fuzz target's entry point carries `// NOLINTNEXTLINE(readability-identifier-naming)`, because libFuzzer fixes the name `LLVMFuzzerTestOneInput`.
- `.clang-format` no longer wraps a comment that starts with `NOLINT`, so that a suppression keeps its check name and reason on one line (coding standard, section 14).
Reason: a sanitizer misses errors in objects that are not instrumented. Replaying the corpus in every build keeps fuzz targets compiling with GCC and on ARM64, and turns every saved input into a regression test.
Consequences: the sanitizer and fuzz builds are x86-64 presets, as section 13 requires; there are no ARM64 variants. Fuzzing needs Clang with its runtime libraries installed. A crash found by fuzzing is fixed together with adding the failing input to the seed corpus.

## D-13: The pipeline: GitHub Actions, ARM64 on a native runner
Date: 2026-11-13
Phase / issue: 0, #5
Status: accepted

Context: every pull request must build and pass its tests on x86-64 and ARM64 (reference 18.1), and the issue leaves open how ARM64 is run. D-6 requires the pipeline to fix one clang-format version.
Options, for ARM64: a GitHub-hosted ARM64 runner (`ubuntu-24.04-arm`), building and testing natively; cross-compiling on x86-64 with the `dev-arm64` presets and running the tests under user-mode emulation; running the whole job in an emulated ARM64 container.
Decision:
- `.github/workflows/pipeline.yml` runs on every pull request and every push to `main`, as six jobs that are six separate checks: `build and test (x86-64)`, `build and test (ARM64)`, `check`, `sanitizers (asan-ubsan)`, `sanitizers (tsan)` and `fuzz`. Each job runs the preset commands of `README.md` and fails when a command fails. No step is allowed to fail.
- ARM64 is built and tested natively on the hosted runner `ubuntu-24.04-arm`, with the same presets (`dev`, `release`) as x86-64. The job first checks that the machine really is `aarch64`.
- Both `build and test` jobs build `dev` and `release`, run their tests, and keep the staged install tree of `release` as an artifact for 14 days.
- The sanitizer and fuzz jobs run on x86-64 only (D-12). Fuzz targets run side by side, one per processor, each for `SCADA_FUZZ_SECONDS` (60 s). Inputs that fail are kept as an artifact.
- Tools: LLVM 18 for clang-format, clang-tidy, clang-query and the fuzzing compiler; GCC 13 and the CMake of the runner image for everything else. Each job prints the versions it used.
- The runner is set up in one place, `.github/actions/setup-build-machine`, which is the only file that names a package manager. It installs LLVM 18 and puts its tools on `PATH` under their plain names. The build scripts stay free of distribution-specific paths and tools.
- Performance tests are not run by the pipeline.
Reason: a native runner executes the tests on real ARM64 processors, so it also covers what emulation does not model faithfully (memory ordering, timing, pointer authentication), and it needs no emulator settings in the build. It is as fast as x86-64, where emulated tests run several times slower. Performance figures from shared runners vary too much between runs to gate on a target; they need a fixed machine.
Measured (run 2 of pull request #30, 2-processor ARM64 runner, 4-processor x86-64 runners): `build and test (ARM64)` 83 s, `build and test (x86-64)` 85 s, both sanitizer jobs about 60 s each, `check` about 2 min. The whole pipeline took 2 min 48 s from start to last job, with the jobs in parallel.
Consequences:
- The cross presets `dev-arm64` and `release-arm64` (D-4) stay for developers; the pipeline does not use them, so nothing checks them automatically.
- The checks gate a merge only when they are marked as required in the protection rule of `main`, which is a repository setting and not a file. Until then the rule in `CLAUDE.md` ("do not merge a pull request whose checks are failing") is the gate.
- The fuzz sample stops within 1 s because its planted bug is found at once. With real targets the fuzz job takes 60 s for every group of as many targets as the runner has processors.
- The hosted runner images change over time. A change of the GCC or CMake version shows in the "Tool versions" step; LLVM stays at 18 until this decision is changed.
- Where and how the performance tests of section 15 of the reference document run is still open. It must be decided before phase 1 is accepted, because phase 1 has the first real targets.
- GitHub's actions `actions/checkout` and `actions/upload-artifact` (both MIT) are tools of the pipeline and are not linked or shipped. The licence report (P0-07) must list them. They are referenced by major version, not by commit hash.

## D-14: The staged tree is an image of the root filesystem
Date: 2026-11-13
Phase / issue: 0, #6
Status: accepted

Context: reference 4.4 fixes absolute paths in `/usr`, `/etc` and `/var`, and `cmake --install build/<preset> --prefix stage` must produce them "under a prefix". A CMake prefix of `/usr` cannot hold `/etc` and `/var`.
Options: the install prefix is the staging root, with destinations `usr/bin`, `etc/scada` and so on; the prefix is `/usr` and staging uses `DESTDIR`; GNU install directory variables.
Decision:
- The install prefix is the staging root. `cmake/ScadaInstall.cmake` holds the destinations, relative to the root. Paths used at runtime, such as `ExecStart=` in a unit, are these destinations with a leading `/` and do not depend on the prefix.
- systemd units go to `usr/lib/systemd/system` and user definitions to `usr/lib/sysusers.d`, which are systemd's own default locations. Both are cache variables (`SCADA_INSTALL_SYSTEMD_UNITDIR`, `SCADA_INSTALL_SYSUSERSDIR`).
- The directories `etc/scada`, `usr/lib/scada`, `usr/share/scada/web` and the four under `var/lib/scada` are installed empty and owned by root. Each is handed to its service's user by the issue that adds that service. `/run/scada` and `/dev/shm/scada` are not in the tree, because they are on memory-backed filesystems; the owning services create them at runtime.
- Service users are declared with `sysusers.d` files, one per service plus one for the `scada` group.
Reason: one command with one prefix gives the whole layout, and the tree can be copied to `/` or wrapped by any package format. `sysusers.d` is part of systemd, which the design already requires, so no distribution's user tool is needed.
Consequences: the build cannot be installed to a prefix such as `/usr/local`. A distribution that builds systemd with other directories sets the two cache variables. Ownership and modes of the data directories, and the permissions of `/dev/shm/scada`, are still to be set, by the service issues and the tag database contract.

## D-15: The systemd notification protocol is implemented in `libscada-service`
Date: 2026-11-13
Phase / issue: 0, #6
Status: accepted

Context: a `Type=notify` service must send `READY=1` and `WATCHDOG=1` to the socket in `NOTIFY_SOCKET`. No dependency list or licence report exists yet (issue P0-07), and `libscada-common` does not exist yet (issue P0-06).
Options: link `libsystemd` (`sd_notify`); implement the documented datagram protocol; start services through a wrapper that notifies for them.
Decision:
- A new library, `libscada-service`, implements the client side of the protocol with plain sockets. It also holds `NotifyListener`, the service manager's side, which tests use in place of systemd.
- Values of `NOTIFY_SOCKET`, `WATCHDOG_USEC` and `WATCHDOG_PID` are validated as untrusted input. A service refuses to start on an invalid value instead of running without notification.
- Until P0-06: the library carries its own `Fd` owner type (the test support library has a second one, `UniqueFd`), and returns `NotifyStatus` and `std::optional` instead of `Status` and `Result<T>`. P0-06 moves `Fd` to `libscada-common` and converts the return types.
- `bind()` needs a `sockaddr*` for a `sockaddr_un`. The conversion goes through `void*` in one function, `bind_unix`, used only by `NotifyListener`. Sending uses `sendmsg()`, which takes the address as `void*` and needs no conversion.
- The environment parser has unit tests for invalid, oversized and overflowing values and the fuzz target `notify_environment_fuzz`.
Reason: the protocol is a few lines, is documented as stable by systemd, and avoids a dependency before the licence report exists. A wrapper would report readiness for a service that is not ready.
Consequences: only the messages the services need are supported (`READY`, `WATCHDOG`, `STOPPING`, `STATUS`); file descriptor passing and `sd_notify` over other socket types are not.

## D-16: Service unit template and the watchdog test hook
Date: 2026-11-13
Phase / issue: 0, #6
Status: accepted

Context: reference 4.2 fixes `Type=notify`, `WatchdogSec`, one user per service and three sandboxing options. It leaves the watchdog interval, the restart policy and the way to hang a service deliberately open.
Options, restart policy: `Restart=on-failure` with systemd's default rate limit; full backoff and restart limit of reference 14.4 now. Options, hang: a signal; a command-line option; a debug build only.
Decision:
- All units are generated from `deploy/systemd/scada-service.service.in` by `scada_install_service(<name> DESCRIPTION <text> WATCHDOG_SEC <seconds> [ARGUMENTS <arguments...>])`. The watchdog interval is set per service; `scada-hello` uses 10 s. Services feed the watchdog at half the interval.
- The template sets `Restart=on-failure`, `RestartSec=1`, and leaves systemd's default rate limit (5 starts in 10 s) in place. Backoff, the restart limit and the system alarm of reference 14.4 are set when supervision is built.
- The template sets `ProtectSystem=strict`, `NoNewPrivileges=yes`, `PrivateTmp=yes` and empty `CapabilityBoundingSet=` and `AmbientCapabilities=`. A service that needs a writable directory or a capability adds it to its own unit when it is built. `NotifyAccess=main` accepts notifications from the main process only.
- `scada-hello` runs as a service only with `--service`; without arguments it still prints the greeting and exits, which the unit, integration and performance tests of issue #4 rely on. As a service it hangs its main loop on `SIGUSR1`. Only `scada-hello` has this hook; it is not part of the template or of `libscada-service`.
- The shared test class `scada::test::ChildProcess` gains `send_signal` and `wait_for_signal_exit`, which the service tests need.
- The test `install-units-verify` runs `systemd-analyze verify --root=<staged tree> --recursive-errors=no`, which needs systemd 250 or newer on the build machine. The test fails if `systemd-analyze` is missing.
Reason: the template holds what every service shares; values that depend on the service stay with the service. A signal needs no change to the unit to trigger a hang on a test machine.
Consequences: every service unit starts from the strictest sandbox and widens it explicitly. Product services get no hang hook (reference 13.6: no debug interfaces in release builds).

## D-17: Include check and POSIX headers
Date: 2026-11-13
Phase / issue: 0, #6
Status: accepted

Context: the first code that uses POSIX interfaces (`libscada-service`) failed `misc-include-cleaner`. The check attributes a name to the header that declares it in this C library, which is often an internal one: `bits/types/sigset_t.h` for `sigset_t`, `sys/poll.h` for `poll`, `stdlib.h` for `WNOHANG`. It then reports the header POSIX names for it (`<poll.h>`, `<sys/uio.h>`) as unused. Separately, `modernize-deprecated-headers` asks for `<csignal>` in place of `<signal.h>`, but `<csignal>` is not required to declare the POSIX signal functions.
Options: disable `misc-include-cleaner`; include the internal headers; tell the check which headers to leave alone.
Decision:
- `.clang-tidy` sets `misc-include-cleaner.IgnoreHeaders` to `bits/.*`, `poll.h`, `sys/poll.h`, `sys/uio.h`, `stdlib.h` and `time.h`. The check neither asks for these nor reports them unused. Project code includes the header that POSIX names for an interface.
- A file that needs POSIX signal functions includes `<signal.h>` with `// NOLINT(modernize-deprecated-headers)` and the reason on that line.
Reason: internal headers differ between C libraries, so including them would tie the code to one (reference 4.1). The check stays on for every other header, including all C++ and project headers.
Consequences: an unused `<poll.h>`, `<sys/uio.h>`, `<stdlib.h>` or `<time.h>` is not reported. The list grows only when a new POSIX header shows the same problem, in the pull request that meets it.
