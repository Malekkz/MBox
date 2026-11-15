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

## D-14: `Status` carries a code and a fixed message; `Result<T>` is our own type
Date: 2026-11-13
Phase / issue: 0, #7
Status: accepted

Context: errors are returned as values (`Status`, `Result<T>`), also by the real-time functions of the tag library, which must not allocate. The documents name the two types but not their content.
Options: a code with an owned `std::string` message; a code with a message that must be a string literal; a code only. For `Result<T>`: `std::expected` (C++23, not available in C++20); a third-party type; our own.
Decision:
- `Status` is a `StatusCode` (14 values, `status.h`) plus a `StaticString`, a view that can only be built from a string literal at compile time. It owns nothing and is trivially copyable. Details that vary at run time go into the log record written where the error is handled.
- `Result<T>` is our own class holding a value or an error `Status`. Calling `value()` on an error, or building a `Result` from an OK status, ends the process through `panic()`; the check stays on in release builds (coding standard, section 7).
- The common library uses the namespace `scada`, not `scada::common`, because these names appear in every interface.
Reason: a literal message makes `Status` usable in real-time code and removes any dangling-view risk, at no cost to callers that only branch on the code. Our own `Result<T>` needs no dependency and no language level beyond C++20.
Consequences: a `Status` cannot carry a file name or a number; callers log them. `Result<T>` can be replaced by `std::expected` if the language level is raised.

## D-15: Logging through a bounded queue, to the journal's native socket
Date: 2026-11-13
Phase / issue: 0, #7
Status: accepted

Context: services log to the systemd journal in a structured format with bounded size (reference 14.6), with a stderr fallback, and logging must not block the caller for more than a bounded time (issue #7). How records reach the journal, and what happens when it stalls, was open.
Options: link `libsystemd` and call `sd_journal_send`; write the journal's native datagram protocol ourselves; print to stderr with a priority prefix only. For a stalled sink: block the caller; drop the oldest queued record; drop the new record.
Decision:
- `Logger::log` copies the record into a pre-allocated queue (default 256 records) under a mutex that is never held during sink I/O, and a worker thread per `Logger` delivers it. When the queue is full the new record is dropped and counted, and one `WARNING` record with the field `DROPPED` reports the loss once the sink recovers.
- A record is limited to a 384-byte message and 8 fields with 32-byte keys and 96-byte values; longer text is cut. Field keys follow the journal's key rule.
- The journal sink speaks the native protocol on `/run/systemd/journal/socket` with a non-blocking datagram socket. No library is linked.
- The journal is used only when `JOURNAL_STREAM` shows that the service manager connected stderr to it; otherwise, and whenever a journal send fails, the record is written to stderr as one escaped text line.
- The record's creation time is sent as the field `SCADA_TIME_NS`, because the journal stamps the later delivery time.
Reason: the native protocol is documented as stable by systemd and needs about 50 lines, while `libsystemd` would be a new dependency to record and cross-compile. Dropping the new record keeps the records that led up to a stall, which are the ones needed to diagnose it, and needs no reordering.
Consequences: records can be lost under overload, and the loss is visible in the log. The size limits keep every record inside one datagram, so the protocol's file-descriptor passing for large messages is not needed. The socket path is fixed by systemd, not by a distribution. The stderr sink bounds its wait with `poll` (100 ms) before a blocking write; another writer that fills the same pipe in between can still delay the worker thread, never a caller. Real-time code must not log (CLAUDE.md); it passes data to a non-real-time thread instead.

## D-16: The staged tree is an image of the root filesystem
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

## D-17: The systemd notification protocol is implemented in `libscada-service`
Date: 2026-11-13
Phase / issue: 0, #6
Status: accepted

Context: a `Type=notify` service must send `READY=1` and `WATCHDOG=1` to the socket in `NOTIFY_SOCKET`. No dependency list or licence report exists yet (issue P0-07).
Options: link `libsystemd` (`sd_notify`); implement the documented datagram protocol; start services through a wrapper that notifies for them.
Decision:
- A new library, `libscada-service`, implements the client side of the protocol with plain sockets. It also holds `NotifyListener`, the service manager's side, which tests use in place of systemd.
- Values of `NOTIFY_SOCKET`, `WATCHDOG_USEC` and `WATCHDOG_PID` are validated as untrusted input. A service refuses to start on an invalid value instead of running without notification.
- The library uses `Fd`, `Status` and `Result<T>` of `libscada-common`. A send returns OK when there is no service manager, because nothing had to be done; `Notifier::enabled()` tells the two cases apart.
- `bind()` needs a `sockaddr*` for a `sockaddr_un`. The conversion goes through `void*` in one function, `bind_unix`, used only by `NotifyListener`. Sending uses `sendmsg()`, which takes the address as `void*` and needs no conversion.
- The environment parser has unit tests for invalid, oversized and overflowing values and the fuzz target `notify_environment_fuzz`.
Reason: the protocol is a few lines, is documented as stable by systemd, and avoids a dependency before the licence report exists. A wrapper would report readiness for a service that is not ready.
Consequences: only the messages the services need are supported (`READY`, `WATCHDOG`, `STOPPING`, `STATUS`); file descriptor passing and `sd_notify` over other socket types are not.

## D-18: Service unit template and the watchdog test hook
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

## D-19: Include check and POSIX headers
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

## D-20: Dependency list and its check are plain JSON and CMake
Date: 2026-11-13
Phase / issue: 0, #8
Status: accepted

Context: reference 16.3 requires a build that fails on a dependency whose licence is not approved, and issue #8 requires it to fail on an unlisted dependency. Neither says where the list lives or how a dependency is detected.
Options: a package manager manifest (vcpkg, Conan); a scanner tool run after the build; a JSON list checked by a CMake module at configure time.
Decision: `third-party/dependencies.json` is the list and `third-party/licence-allowlist.json` the allow-list. `cmake/ScadaDependencies.cmake` validates both at configure time, then checks every link item of every target of this project: it must be a target of this project, a `link_names` value of a list entry, or a platform library. A directory under `third-party/` must be named after an entry. The two files are configure dependencies, so a build after an edit configures again.
Reason: it needs no tool beyond CMake, which the build already requires, and no package manager, which the reference forbids relying on.
Consequences: build code that fetches a dependency reads its version from the list with `scada_dependency_version`, as `cmake/ScadaTesting.cmake` does for GoogleTest, so the version is written once. Every dependency must be linked by a name, including header-only ones (an `INTERFACE` target). Not detected: headers used through an include path alone, and what vendored or fetched third-party code links itself; review covers these. Link items written as generator expressions other than plain wrappers are rejected.

## D-21: What counts as a dependency, and the first allow-list
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

## D-22: Bill of materials format and where the reports go
Date: 2026-11-13
Phase / issue: 0, #8
Status: accepted

Context: reference 13.6 requires a software bill of materials and 16.3 a licence report, without naming formats or locations.
Options: CycloneDX JSON; SPDX JSON; a project-specific format.
Decision: the build writes `build/<preset>/reports/sbom.cdx.json` (CycloneDX 1.5 JSON) and `build/<preset>/reports/licence-report.md`, and installs both to `usr/share/doc/scada/` of the staged tree (D-16), the usual place for documents that belong to a product, which reference 4.4 does not list. Dependencies with scope `build` or `test` are in the bill of materials with CycloneDX scope `excluded`. The file has no timestamp and no serial number.
Reason: CycloneDX needs no generated identifiers or timestamps, so the same input gives the same file, and common scanners read it.
Consequences: the pipeline uploads `build/release/reports/` of each architecture as the artifact `dependency-reports-<architecture>`. The generated file is checked by tests for content, not against the CycloneDX schema, because no validator is available offline.

## D-23: Tag database contract review: wake-up by `scada_tag_notify`, and the other review outcomes
Date: 2026-11-13
Phase / issue: 1, #9
Status: accepted

Context: the tag database contract draft 0.1 was reviewed against reference section 5 before phase 1 code starts. The draft had tag writes and queue posts wake waiting consumers, which needs a system call; reference 5.7 and `CLAUDE.md` forbid system calls in `write` and `post_request`. The draft also listed three refinements of the reference and five open points without saying where each is settled.
Options: for the wake-up: allow one non-blocking wake call inside `write` and `post`; never wake and let consumers poll on a short timer; wake once per cycle through a separate call. For a post to an event queue whose consumer is down: reject at once; accept until the queue is full. For the open points: fix the proposed values now; one separate experiment issue; assign each to the phase 1 issue that builds the part.
Decision (by the product owner):
- Writers never wake consumers. A process calls `scada_tag_notify()` once per cycle or scan, outside its real-time section, and every consumer wait has a timeout capped at 100 ms.
- The three refinements of contract section 17 are confirmed. The `down` flag is in the process table, as in the reference, so that processes without tags have one too.
- A post to an event queue whose consumer is down is rejected at once with `SCADA_E_PEER_DOWN`.
- Each open point is settled in a named phase 1 issue against a pass criterion (contract section 16).
Reason: the real-time rule stays without an exception, and a consumer sees a change at most one cycle late. Rejecting at once is the strict reading of "no audit, no action": no command is accepted while its audit record could exist only in memory.
Consequences: a process that forgets `scada_tag_notify` still delivers, up to 100 ms late. While `scada-audit` or a journal consumer restarts, commands are refused. The review also changed the string write order, record ids, the ring `epoch` and several checks; the contract's change log (section 18) lists them. The contract is version 0.2, layout version 1.

## D-24: Tag database contract, second review: owners are threads, commands at most once, and the other outcomes
Date: 2026-11-13
Phase / issue: 1, #9
Status: accepted

Context: draft 0.2 of the tag database contract was reviewed independently from three sides (SCADA practice, C++ and lock-free correctness, Linux). All three found it fit for phase 1 only after changes. Six points needed a decision; the rest were corrections.
Options:
- Several writing threads in one process (control tasks, driver channels) against single-writer rings and queues: one ring and queue set per writing thread; all publication through one thread; a multi-producer design.
- Command queue delivery: at least once with duplicate suppression in the driver; at most once.
- Automatic control writes and "no audit, no action": exempt source AUTO; require the record for every source; leave it to phase 5.
- An owner shrinking its region file kills mapped readers: accept; hand out sealed memory from `scada-tagd`.
- Wake-up: futex with `futex_waitv` and an in-process bridge to a file descriptor; eventfd passed by `scada-tagd`.
- Scope: everything in the pull request of #9; blockers only.
Decision (by the product owner):
- An owner is a writing thread. A process has one owner, ring and queue set per writing thread. The handle is 10 bits owner, 22 bits slot.
- Command queues deliver at most once: the driver acknowledges before executing, and on attach drains pending requests with status `ABORTED`. A request lives at most 60 seconds.
- Write requests of every source, AUTO included, need their event record accepted first. While the consumer of that event queue is down, `scada-control` cannot post write requests and outputs follow their configured hold or safe behaviour.
- The truncation risk is accepted for layout version 1 and written into the contract.
- Futex is the primary wake mechanism, with wait sets on `futex_waitv` and an in-process eventfd bridge for threads that wait on sockets. Minimum kernel Linux 5.16.
- All review findings are fixed in the pull request of #9 (contract 0.3; its section 18 lists them).
Reason: one writer per ring keeps every path lock-free and the real-time rule intact. A command that may run twice or late is worse than one that is reported as aborted (reference 14.4). Every change to the plant leaves a record, whatever its origin (reference 3.3).
Consequences: the tag-set definition must name the writing threads. A restart of the journal or audit consumer stops automatic control until it is back; the event volume of automatic writes has to be sized when the journal is designed (phases 5 and 7). A driver restart aborts commands in flight, and the operator repeats them. The base directory must be created by the platform at boot, which phase 0 packaging (#6) has to provide. Corrections that needed no decision, among them the memory fences for ARM64 and `SCADA_E_BUSY` in place of a false `OWNER_DOWN`, are listed in the contract's change log.

## D-25: Where the tag database layout lives and how it is checked
Date: 2026-11-13
Phase / issue: 1, #10
Status: accepted

Context: issue #10 puts the structures of the tag database contract (sections 4 to 10) into code. The contract fixes the layout but not where the header lives, how the array fields are written, the names of enumerations it gives only as comments, or the form of the static checks.
Options, location: a public header of `libscada-tag`; a private header in its `src/`. Options, array fields: C arrays as the contract writes them; `std::array`. Options, checks: one `static_assert` per number; constant tables checked by one `static_assert` per structure.
Decision:
- `libs/scada-tag/src/layout.h` holds the structures and is private to `libscada-tag`. The public headers are `scada/tag/types.h` (tag type, quality, quality flags, write request and result enumerations) and `scada/tag/handle.h`. The name hash is private too (`src/name_hash.h`).
- Array fields (`name`, `reserved`, `config_hash`, `data`, `payload`) are `std::array`. The checks prove the layout is the one of the C array.
- A structure the contract writes without `alignas` has its natural alignment: 8, 4 (`ProcessEntry`, `QueueEntry`) or 2 (`StringSlot`) bytes.
- Each structure has a constant table of `LayoutCheck{actual, contract}` entries for its size, its alignment and every field offset, and two `static_assert`: the table holds, and the structure is standard-layout, trivially copyable and has unique object representations (no padding).
- Enumerations the contract gives only in field comments are named `GenerationState`, `ProcessState`, `QueueKind`, `ProcessFlag`, `TagFlag`, `WriteOperation`, `WriteSource` and `WriteStatus`, each with the underlying type of its field.
- `unpack_handle` rejects owner index 1023 and slot 4,194,303 as well as `SCADA_INVALID_HANDLE`, because section 14 allows 1,023 owners and 4,194,303 tags per owner.
- The values of `EventRecord.kind` (ALARM, COMMAND, AUDIT, SYSTEM) are not defined in code: the contract gives no numbers. They must be written into the contract before the first code that posts an event (issue P1-08).
Reason: a private header makes "no other code may depend on the binary layout" a build error, not a rule to remember. `std::array` gives checked access under the library assertions and needs no suppression for C arrays. The lint rule against unnamed numbers accepts a number only in a constant initialiser, and a table keeps each offset beside its field without several hundred suppressions.
Consequences: the builder of the `header` region (issue P1-03) lives in `libscada-tag`, or this decision is revisited. A failed layout check names the structure, not the field; the unit test `Conformance01Layout` names the field. `layout.cpp` suppresses `misc-include-cleaner` once, because it includes `layout.h` only for its checks. `layout.h` carries 18 suppressions: 5 of `performance-enum-size`, because the underlying type is the type of the field, and 13 of the magic-number checks, one for the size of each `reserved` array.

## D-26: Tag-set definition, its temporary file, `config_hash` and the header builder
Date: 2026-11-14
Phase / issue: 1, #11
Status: accepted

Context: issue #11 needs a tag-set definition, a temporary file format for it, `config_hash` and the image of the `header` region. The contract (sections 2, 5 and 14) fixes the layout and the limits but not how indexes are assigned, what exactly is hashed, the content of the padding and of the name pool, or the file format. The reference document expects a JSON parser and a hash from third-party projects but leaves the choice open.
Options, file format parser: a third-party JSON library; a small parser for the subset the file needs. Options, hash: a third-party cryptography library; SHA-256 written here. Options, order independence: hash and build in the order given and require a sorted file; sort inside the library. Options, user of a process: user name resolved when the header is built; numeric uid in the definition.
Decision:
- `scada::tag::TagSet` (`scada/tag/tag_set.h`) is the one interface between configuration and the data plane. `parse_tag_set_json` (`scada/tag/tag_set_json.h`) is the only code that knows the temporary file; phase 2 replaces it.
- The file is read by a parser written here for a subset of JSON: objects, lists, strings of printable ASCII without escape sequences (at most 127 bytes), whole numbers from 0 to 4,294,967,295, `true` and `false`. Unknown, repeated and missing keys are errors. No dependency is added.
- SHA-256 is implemented in `libs/scada-tag/src/sha256.cpp` and checked against the vectors of the standard. It identifies a tag set; it protects no secret.
- The library sorts: processes, owners and queues by name, their position being the process index, owner index and queue id; the slots of an owner are its tags by name. `config_hash` is the SHA-256 of the canonical encoding written in `libs/scada-tag/README.md`, which starts with a name and a version of the encoding. Generation and creation time are not hashed.
- A process is given with its numeric uid. An owner entry gets the uid of its process.
- A queue's kind is the prefix of its name (`events.`, `cmd.`, `result.`), and its record size follows from the kind. The producer and consumer are process names. The rest of the name is not checked against them.
- Layout details the contract leaves open: each slot table is padded with zero bytes to a multiple of 8 bytes; an owner without tags has an empty slot table; the name pool holds the names in directory order without terminators, and `name_pool_size` is the number of name bytes; the file ends at the next multiple of 8 bytes; in the `RegionHeader` of `header`, `owner_index` is 0 and `queue_id` is 0xFFFF. The builder leaves `ready` 0 and `state` BUILDING for `scada-tagd` to set.
- Rules added to those the contract states: `retain_now` needs `retain`; a string tag cannot be `writable_by_command` (contract 9.4: strings cannot be commanded); a queue capacity is a power of two up to 2,147,483,648, default 1,024; uid 4,294,967,295 is refused; heartbeat times are at least 1 ms. A change ring capacity that is omitted is 65,536 and is not raised automatically: an owner with more than 32,768 tags must state it.
- An error is a `Status` with a fixed message plus a `std::string detail` filled by the function, because `Status` cannot carry text made at run time (D-14). A name from outside is repeated in the detail only after it passed the name grammar.
- The time target of the issue (50,000 tags in under 1 second) is a performance test, `scada-tag-header-perf-test`, run by `ctest --preset perf`.
Reason: the file is temporary, so a dependency chosen for it would be chosen for the wrong reasons; phase 2 picks the configuration parsers with the JSON Schema work. A parser for this subset and SHA-256 are each a few hundred lines with published test vectors or a fuzz target. Sorting inside the library makes "same tag set, same hash, same handles" hold for every producer of a `TagSet`, not only for a well-behaved file. A numeric uid keeps the builder free of system calls and its result reproducible in tests.
Consequences: handles and queue ids depend on names only, so they are stable across a restart of `scada-tagd` with the same tag set, and change for tags that sort after an added or removed tag. `scada-tagd` (issue #12) calls `config_hash`, then `build_header`, writes the bytes and sets `ready` and `state`. The layout details above, the canonical encoding and the added rules must be written into the contract before it becomes 1.0 (issue P1-13); until then they are stated here and in the library's `README.md`. The numeric uid in the file is replaced by a user name when the real configuration arrives in phase 2. If a third-party cryptography library is added later for signing, `sha256.cpp` is replaced by it.
Review: a committee of three reviewers (C++, SCADA architecture, Linux systems) examined each point of this decision on 2026-11-14 and confirmed all of them. Their notes that later work must take up:
- Contract text for 1.0 (P1-13): the items above; "sorted by name" means byte-wise; an offset may equal the file size when the part it points to is empty; `DirectoryEntry.name_offset` is a byte offset into the pool, not a multiple of 8; a real limit for queue capacity; when the encoding version of `config_hash` changes; name order is the builder's rule for layout version 1, not a promise to consumers.
- `scada-tagd` (#12): the tag set must hold the process of `scada-tagd` itself; the library needs public functions to publish a header and to set `ready`, `state` and `down`, because the layout is private; reuse of a generation needs `ready` and RUNNING as well as an equal hash; the sum of all region sizes is checked before a generation is published; removal of stale temporary files matches the exact name.
- Attach (#13): accept an offset equal to the file size for an empty part.
- Tag read and write (P1-06): which string slot a STRING tag gets. Proposed: its rank among the owner's STRING tags in name order.
- Retained values (P1-09): a tag that moves to another owner, or whose owner is renamed when its process gains a second writing thread, must find its retained value.
- Phase 2 configuration: refuse uid 0 and two processes with one uid outside tests; apply the real-time heartbeat default (500 ms), which today only the temporary file parser applies; tell the user that any change of a hashed setting restarts the data plane.

## D-27: `scada-tagd`: where its logic lives, how it starts a generation and how it judges liveness
Date: 2026-11-14
Phase / issue: 1, #12
Status: accepted

Context: issue #12 adds `scada-tagd`: it creates or reuses the generation, owns `header` and `proc.tagd`, and sets and clears the `down` flag. The contract (sections 3, 10 and 11.1) fixes the files, the permissions and the liveness rules. It leaves open where the code lives while the layout is private to `libscada-tag` (D-25), what exactly "reuse" and "live process" test, how two instances are kept apart, and several details of the sweep and of the loop. The issue names "conformance tests 4 (owner-down part) and 11", which are numbers of contract 0.1.
Options, location: `scada-tagd` includes the private headers of `libscada-tag`; a public class in `libscada-tag` for the side of `scada-tagd`. Options, reuse: equal `config_hash` and layout version; also `ready` and RUNNING; also a header equal byte for byte to the one the tag set builds. Options, live process of an old generation: a fresh heartbeat in a `proc` region; a held lock on a `proc` region. Options, second instance: rely on the lock on `proc.tagd`; a lock on the base directory.
Decision:
- `scada::tag::GenerationHost` (`scada/tag/generation_host.h`) is the side of `scada-tagd` and holds everything that touches the layout or the database files. `libscada-tagd` holds the service: command line, tag-set file, log, service manager and the loop. Private to `libscada-tag`: `shared_memory.h` (the reviewed helpers that place structures on mapped memory, with the only `reinterpret_cast`), `region_file` (creation and the checks of contract 3.1 and 3.2) and `proc_region` (own region and probe). `MappedRegion` is added to `libscada-common`.
- The tag set must hold a process named `tagd` whose uid is the effective user id `scada-tagd` runs as. That user id is "the uid of the user `scada-tagd`" for the checks of contract 3.1; no user name is looked up.
- `scada-tagd` reads `/etc/scada/tag-set.json`; `--tag-set` and `--base-dir` replace the file and the base directory.
- `scada-tagd` holds an exclusive lock (`flock`) on the base directory for its whole life, besides the lock on `proc.tagd`. A second instance fails before it reads or changes anything.
- A generation is reused only if its directory has the owner and mode of the contract, its `header` passes every check of contract 3.1, is `ready` and RUNNING, has the same `config_hash`, and equals byte for byte the header this library builds for the tag set, apart from `ready`, `state`, `created_time_ns` and the `down` flags.
- "No live process" in contract 11.1, step 3, means: no `proc.*` file in the directory is locked. The lock is the one a process holds for its whole life (contract 10), so the test needs no header of the old generation.
- A new generation gets the highest number found in the base directory plus 1, also when that directory is then removed.
- Before a generation is created, the sum of the sizes of all its regions is compared with the free space of the file system of the base directory. If it does not fit, the start fails and nothing is created.
- After `current` moved, the state of the generation it pointed to is set to RETIRING (contract 11.3, step 1). Its removal while `scada-tagd` runs comes with issue P1-09; until then it is removed by the next start that builds a generation, once no process is attached.
- A region gets its real name with `renameat2` and `RENAME_NOREPLACE`, so that a file that is already there is never replaced.
- The sweep of contract 3.1 leaves a file `.tmp.<region>.<digits>` of a listed region, owned by the user of that region: it is a region being created (contract 3.2). An entry that cannot be removed is reported once. An entry under a listed name with the right owner is left, whatever its type.
- If `current` exists and is not a symbolic link, `scada-tagd` does not start. A link with another target than a generation name is replaced.
- Liveness (contract 10.1): the rules are tested in the order region, state, heartbeat, and the first that holds is the reason. A heartbeat exactly as old as the timeout is not yet down. A heartbeat newer than the time of the check is fresh. `scada-tagd` never sets its own `down` flag. Changes are logged as `process down` (WARNING) and `process up` (NOTICE); the first check of a new generation logs at INFO, because no process has attached yet.
- The loop runs every `heartbeat_check_ms`, or more often if a third of the heartbeat timeout of `tagd` or a quarter of the watchdog interval is shorter. Each round writes the heartbeat and checks liveness; the watchdog is fed from the same loop, four times in its interval.
- Open point of contract section 16, heartbeat timeout defaults: 3,000 ms and 500 ms are kept. Conformance test 5 passes with them for the part of `scada-tagd`; test 26 confirms them in issue P1-12.
- The issue's tests 4 and 11 are tests 5 and 20 of contract 0.3. This issue covers the part of `scada-tagd` in tests 5, 6, 9, 10, 11 and 20; the part of the reading library follows with attach and read (issues #13 and P1-06).
- `/dev/shm/scada` is created at boot from `usr/lib/tmpfiles.d/scada-tagd.conf` (`d /dev/shm/scada 2750 scada-tagd scada -`), installed with the service.
Reason: a public class keeps "no other code may depend on the binary layout" a build error also for `scada-tagd`. An equal hash does not prove that the header is intact or was built by the same version of the builder; a byte comparison does, and costs one pass over the header at start. A lock is released by the kernel when its process dies, so it cannot be stale as a heartbeat of unknown timeout can. The lock on the base directory closes the time between two instances reading `current` and attaching `proc.tagd`. Without the exception for temporary files, the sweep would remove a region every 250 ms while its process is still filling it.
Consequences:
- Contract text for 1.0 (issue P1-13): the lock on the base directory; the reuse test; "live process" as a held lock; the numbering of generations; the exception for temporary files in the sweep; `renameat2` without replacement; that `scada-tagd` takes its own user id; the order of the liveness rules and the two limits of the heartbeat age.
- Attach (issue #13) builds on `region_file` and `proc_region`. Until then the integration tests use the helper `scada-tag-test-peer`, which attaches a `proc` region through the same code.
- The free-space check counts an old generation that is still there as used space, so a change to a much larger tag set can be refused until the old generation is gone.
- Conformance test 11, second part, is shown with a limit on the file size and with the free-space check. A file system that is really full needs the right to mount one and is left to the conformance suite (issue P1-12).
- A test found that stop signals must be blocked before the logger starts its thread; `scada::tagd::run` does so, and every service with a logger must.
- A test found that a watchdog message in every 50 ms round fills the queue of a service manager that does not read at once (10 datagrams by default on Linux), so that the stop message is lost. The watchdog is therefore fed four times in its interval, not in every round.
- The numeric uid in the tag-set file is replaced by a user name with the configuration of phase 2 (D-26).
- New suppressions: `scada-banned` with `cppcoreguidelines-pro-type-reinterpret-cast` once in `shared_memory.h`; `scada-banned` with the cast checks once for `MAP_FAILED` in `mapped_region.cpp`; `cppcoreguidelines-pro-type-vararg` for `open` and `openat` (twice in `region_file.cpp`, once in `tagd.cpp`, four times in test code); `concurrency-mt-unsafe` for `readdir` and two array checks for `d_name` in `region_file.cpp`; `modernize-deprecated-headers` for `<stdio.h>` and `<signal.h>`. In test code, as in the existing tests: the name of the libFuzzer entry point, `getenv`, and the use of a moved-from object in the test of `MappedRegion`.

## D-28: Attach: the C interface so far, the order of its steps, own regions, and regions of other processes
Date: 2026-11-14
Phase / issue: 1, #13
Status: accepted

Context: issue #13 adds `scada_tag_attach`, `scada_tag_detach`, `scada_tag_heartbeat` and `scada_tag_status`: a process joins the current generation, creates or reuses and repairs its own regions, and maps the regions of other processes read-only. The contract (sections 3, 6.3, 11.2 and 13) fixes the checks, the repair and the function names. It leaves open the content of `scada_status`, how return codes map to what attach can meet, several details of the regions, and how the read-only mappings behave while their files change. The issue names "conformance tests 5, 7 and 8", which are numbers of contract 0.1.
Options, where attach lives: logic in the C functions; a private C++ class with the C functions as a thin layer. Options, lock: taken after the own regions, as contract 11.2 lists it; taken first. Options, header of a client: trusted because `scada-tagd` wrote it; validated in full before use. Options, a read-only mapping whose file was replaced: unmapped at once; kept until detach. Options, `errno` with `SCADA_E_IO`: left as the last system call set it; recorded where the error is reported.
Decision:
- `scada/tag/scada_tag.h` is the C interface, usable from C and C++. It holds the 15 return codes of contract 13.3 and the five functions of this issue; the later issues add theirs. The logic is the private class `Attachment` (`src/attachment.h`); the C functions check pointers, convert `Status` to a return code and let no exception out (a failed allocation is `SCADA_E_IO` with `ENOMEM`). A public C++ wrapper is added when the first C++ service needs one.
- `scada_status` is 32 bytes: `generation`, `generation_state` (1 to 3, or 0 for a value that is no state), `process_index`, `tagd_down`, `regions_created`, `regions_reused`, `records_repaired`, and two reserved fields that are 0. `scada_tag_status` fills it and returns `SCADA_E_RETIRING` if the generation is retiring, otherwise `SCADA_E_TAGD_DOWN` if `scada-tagd` is down by the first three rules of contract 10.1, otherwise `SCADA_OK`.
- `scada_tag_heartbeat` returns `SCADA_E_RETIRING` when the generation is retiring (contract 11.3: checked on every heartbeat); the heartbeat is written all the same.
- Return codes of attach: `SCADA_E_NOT_FOUND` only for a process that is not in the process table. `SCADA_E_LAYOUT` for another magic or layout version, and also for a header or a `current` that is not valid. `SCADA_E_IO` with `ENOENT` for a missing base directory, `current` or header; with `ENOSPC` when the file system has no room; with `EAGAIN` for a generation that is BUILDING. `SCADA_E_INVALID` for a NULL argument, a name that is no process name, or a `tagd_uid` that is neither -1 nor a uid.
- `errno` for `SCADA_E_IO` is the error number the library recorded where it reported the failing call (`errno_text` in `region_file`), kept per thread, because a `Status` cannot carry it (D-14) and clean-up calls after the failure change `errno`.
- The lock on `proc.<process>` is taken before the other own regions are looked at, not after as contract 11.2 lists it. A second process of the same name then fails before it can remove or repair a region the first one is using.
- The generation directory is checked like the base directory: owned by the uid of `scada-tagd`, mode `3770`.
- The header is validated before use (coding standard, section 9): everything listed in `read_header` (`src/header_view.h`). The process, owner and queue tables are copied out after validation; the tag directory and the slot tables are checked entry by entry when they are used. A table offset may equal the file size when the table is empty (D-26). The file is at most 256 MiB; a header of the limits of contract section 14 takes about 156 MiB.
- Own regions, in addition to the checks of contract 3.1: a reused `tags` region must have the type of the tag directory in every record, a reused ring the capacity of the owner table, a reused queue the capacity and record size of the queue table. Otherwise it is removed and created again, like a region of the wrong size.
- The `RegionHeader.owner_index` of a queue region is the process index of its producer. The value of a STRING record is its string slot, the rank of the tag among the owner's STRING tags in slot order, written when the region is created (proposed in the review of D-26). The repair finds the slot by the same count, not from the record.
- The `epoch` of a ring is incremented at every attach that reuses the ring, whether a record was repaired or not. A new ring starts at 0.
- Regions of other processes (`PeerRegions`): one slot for each region the header lists. A slot is mapped when it is first asked for. A region is looked at again at most once a second, and not only while it is missing: a mapped region whose file is gone, was replaced or was shrunk is dropped or mapped anew. Until `scada_tag_notify` exists (issue P1-07), `scada_tag_status` does this. A mapping is kept until detach, because a reader in a real-time function may still hold it; a region may be mapped anew 16 times, after that it reads as missing until the process attaches again.
- Own and other regions are brought into memory with `madvise` (`MADV_POPULATE_WRITE`, `MADV_POPULATE_READ`, Linux 5.14), which returns an error where touching the pages would end the process with a signal.
- Attach starts again at most 5 times when it finds a RETIRING generation, without waiting: `scada-tagd` switches `current` before it retires a generation (D-27).
- Left to the issue that builds the part: retained values (P1-09); draining the `cmd` queues, and the cursors of a new `proc` region in an existing generation (P1-08); the clock check of contract 13.1 and its field in `scada_status` (P1-06); `mlockall` stays with the process, as the contract says.
- The issue's tests 5, 7 and 8 are tests 7, 9 and 12 of contract 0.3. This issue covers the part of attach in tests 7 (repair, readers keep their mappings, the epoch), 9 (wrong user, symbolic link, base directory), 10, 11 (modes) and 12. `SCADA_E_GAP` in test 7 comes with the rings (P1-07); `SCADA_E_NOT_OWNER` in test 8 with write (P1-06), where the read-only mapping is already in place.
- Test 12 is shown from the side of the file: a header whose layout version is 2 is refused by this library. A library built with another layout version is not built by the tests.
Reason: a private class keeps the layout out of every public header and lets unit tests reach the regions, while the C functions stay a few lines each. Taking the lock first costs nothing and closes the time in which two processes of one name could both repair. A header is the one region every process reads; a bug or a crash of `scada-tagd` must not become a wild read in every service. Keeping replaced mappings is the only way a real-time reader never touches unmapped memory without a lock; the limit bounds what a misbehaving owner can take from its readers.
Consequences:
- Contract text for 1.0 (issue P1-13): the definition of `scada_status`; the return codes above, among them `SCADA_E_LAYOUT` for a header that is not valid; the lock before the own regions; the check of the generation directory; the content checks of own regions; `owner_index` of a queue region; the string slot rule; the epoch at every reuse; the once-a-second look at mapped regions as well as missing ones, and the limit of 16; `MADV_POPULATE` and with it Linux 5.14, below the minimum of 5.16.
- A process that is attached cannot be handed a new uid for `scada-tagd`; the lookup of the user `scada-tagd` happens once, at attach.
- `scada-tag-test-peer` now attaches through the C interface; the integration tests of `scada-tagd` use it as before.
- GoogleTest's death tests and parameterised tests are not used: their macros contain `goto` and `new`, which the banned-construct check reports (D-8, D-10). That a store through a read-only mapping ends the process is therefore not tested; `MappedRegion` maps with `PROT_READ` alone.
- Under user-mode emulation of ARM64 (`qemu-aarch64`), `MADV_POPULATE_READ` on a file shorter than its mapping reports success, so the unit test `MappedRegion.PrefaultFailsWithAnErrorWhenTheFileIsTooShort` fails there. The pipeline runs ARM64 natively (D-13).
- New suppressions: `cppcoreguidelines-avoid-non-const-global-variables` once in `region_file.cpp` (the per-thread error number); in `scada_tag.h`, for the C declarations the contract fixes: `performance-enum-size` twice, `modernize-use-using` three times and `readability-identifier-naming` three times; `readability-identifier-naming` once in `c_api.cpp` (`struct scada_ctx`); `cppcoreguidelines-pro-type-vararg` for `open` once in the test helper; the name of the libFuzzer entry point in `header_region_fuzz.cpp`.

## D-29: Tag read and write: the C interface, the sequence lock in code, the overlay, and how real-time safety is tested
Date: 2026-11-14
Phase / issue: 1, #14
Status: accepted

Context: issue #14 adds `scada_tag_resolve`, `scada_tag_info`, `scada_tag_read`, `scada_tag_read_many`, `scada_tag_write` and the string functions, with the sequence lock of contract sections 6.1, 6.2 and 7 and the overlay of the owner state, and a test that the real-time safe functions make no system call and no allocation. The contract fixes the protocol and the function names. It leaves open what `scada_tag_info_t` holds, what several functions return in which case, what a read gives when an owner died in the middle of a write, how the "pause instruction" is written in a code base that allows no architecture-specific code outside the I/O HAL, and how the instrumentation of conformance test 22 works. The issue names "conformance tests 2, 3, 4, 6 and 13", which are numbers of contract 0.1.
Options, access to the fields of a record: as 64-bit words through a cast, as the contract words it; field by field through `std::atomic_ref`. Options, a record left with an odd sequence number by an owner that is down: `SCADA_E_BUSY` for ever; BAD with OWNER_DOWN and no value. Options, the `down` flag of `scada-tagd` in the overlay: used as a fourth rule; not used. Options, pause: none; `std::this_thread::yield` (a system call); one function that names the instruction. Options, finding system calls: a seccomp filter in the process; syscall user dispatch (x86-64 only); tracing a child process with ptrace. Options, finding allocations: counting in replaced allocation functions; comparing allocator statistics.
Decision:
- The sequence lock is in `src/tag_record.cpp`, the only code that reads fields while another process writes them. Every field of a record and of a string slot is loaded and stored through `std::atomic_ref` with relaxed order, field by field, not as 64-bit words. The loads and stores of `seq` and the two fences are those of the contract, in its order. A read tries 64 times.
- The fences are `fence_release` and `fence_acquire` in `src/shared_memory.h`. In the ThreadSanitizer build they are empty: the compiler refuses `std::atomic_thread_fence` there, and that build runs all atomic accesses in one total order. Because every access is atomic, no access is a data race, ThreadSanitizer reports nothing and no suppression file exists. ThreadSanitizer does not check the order of the accesses; the tests with derived values and the model check of conformance test 4 do. The ThreadSanitizer build is for x86-64 only and does not compile for ARM64, where a lock without its fences would be wrong.
- The pause is `scada::cpu_relax()` in `scada/common/cpu_relax.h`: `pause` on x86-64, `yield` on ARM64, written as a standard `asm` declaration. It is the one place outside the I/O HAL that names a processor instruction; any other target fails to compile.
- At attach the library reads the tag directory through the slot table of every owner, not only of its own, and keeps the type of every slot (1 byte for each tag) and the STRING slots of every owner. A handle is then checked with two comparisons, and a read knows the type without the header. A slot without a valid directory entry fails attach with `SCADA_E_LAYOUT`.
- Overlay: the owner counts as down by the first three rules of contract 10.1, evaluated at every read from the `proc` region of its process. The `down` flag of `scada-tagd` is not used: it is the same judgement made up to one check interval earlier, and a flag left by a `scada-tagd` that has stopped would keep a restarted owner down.
- What a read returns: the last value with quality BAD and OWNER_DOWN added when the owner is down; type of the tag, value 0, times 0, quality BAD and only OWNER_DOWN when the `tags` region is missing, or when all 64 attempts found the record mid-write and the owner is down; `SCADA_E_BUSY` when all attempts found it mid-write and the owner is not down. `age_ns` is never negative. (D-30 adds what the output holds with an error.)
- A tag of another process reads as BAD with OWNER_DOWN until it was resolved in the same context, because resolve is where the regions are mapped (contract 11.2).
- A record is input. `SCADA_E_LAYOUT` is returned when a record does not hold the type of its tag, a quality, only known quality flags, 0 or 1 for a BOOL, or the number of its string slot for a STRING; and when a string slot holds a length above 250.
- Write: `SCADA_E_NOT_OWNER` before the value is looked at; `SCADA_E_TYPE` for another type and for `scada_tag_write` on a STRING tag; `SCADA_E_INVALID` for a quality or flag that is none and a BOOL that is neither 0 nor 1. With `source_time_ns` 0 the source time is the update time and LOCAL_TIME is set. `scada_tag_write_many` checks every pair first and writes all or nothing. The value of a STRING tag counts as changed when its text differs.
- Strings: `scada_tag_write_string` refuses text that is not well-formed UTF-8 or longer than 250 bytes; the check (`src/utf8.h`) has the fuzz target `utf8_fuzz`. `scada_tag_read_string` returns the bytes the owner stored, not NUL-terminated; with a buffer that is too small it returns `SCADA_E_INVALID` and the length. A reader does not check UTF-8. The string functions are not real-time safe, as in the contract.
- `scada_tag_read_many` returns `SCADA_OK` or the code of the first tag that was not read; the status array may be NULL.
- `scada_tag_info_t` is 168 bytes: type, flags, whether the caller owns the tag, owner index, process index, tag name and owner name.
- `scada_status.reserved0` becomes `clock_syscall`: 1 if the name of the kernel's clock source is not one the vDSO reads (`tsc`, `kvm-clock`, `hyperv_clocksource_tsc_page`, `arch_sys_counter`) or one clock read takes more than 200 ns (the fastest of 4 batches of 256 reads). Checked at attach and, until `scada_tag_notify` exists (issue P1-07), at most once a minute in `scada_tag_status`.
- `scada_tag_write_many` is part of this issue although the issue does not list it: contract 0.3 added it after the issue was written, and no later issue names it.
- Conformance test 22: the helper `scada-tag-rt-probe` calls every real-time safe function between two markers. The test runs it as a child process traced with ptrace (`scada/test/syscall_trace.h`, Linux 5.3) and fails on any system call between the markers; the helper counts allocations there in replaced global allocation functions that hand out memory from a fixed block. Two further tests make a system call and an allocation on purpose and expect them found. If the library reports `clock_syscall`, clock reads are not counted. The test is registered only in a native build without a sanitizer.
- Conformance test 2 runs 3 s in the pipeline; `SCADA_CONFORMANCE_SECONDS=600` gives the 10 minutes of the contract.
- The issue's tests 2, 3, 4, 6 and 13 are tests 2, 3, 5 with 6, 8 and 22 of contract 0.3.
Reason: field-wise atomic access gives the same guarantee as the word-wise copy of the contract without a cast, and needs no suppression of a race report. A reader that waited on `SCADA_E_BUSY` for a dead owner would never report it, which conformance test 5 forbids. Tracing a child needs no privilege and no architecture-specific code, and sees a system call made inside the vDSO, which a wrapper around library functions would miss.
Consequences:
- Contract text for 1.0 (issue P1-13): field-wise access in place of "one 64-bit word at a time"; the definition of `scada_tag_info_t` and of `clock_syscall`; the return codes above, among them `SCADA_E_LAYOUT` for a record that is not valid; what a read gives for a missing region and for a record left mid-write by a dead owner; that the overlay uses the first three rules of 10.1 only; that a tag must be resolved in the context that reads it; the rule for the clock check.
- The retry limit of 64 is confirmed in issue P1-12 (contract section 16). Measured here on x86-64 only, release build, 4 processors (2.1 GHz): the reference load for 10 minutes gave `SCADA_E_BUSY` for 22 of 62.0 billion reads by 4 readers, against the criterion of 1 in 1,000,000; one tag written in a tight loop with 4 readers on the same 4 processors, debug build, gave it for about 1 read in 9.
- Not done here: the change entry of a write (issue P1-07 adds it where `write_record` returns that the value changed); conformance test 4, the model check of the sequence lock, for which the pipeline has no tool yet (it has an issue, see D-30). The debug-build check of contract 13.2 was added after the review (D-30).
- Attach reads the whole tag directory: about 1 ms for 50,000 tags.
- New suppressions: `scada-banned` and `cppcoreguidelines-pro-type-reinterpret-cast` once in `shared_memory.h` (`text_at`, the name pool read as characters); in `scada_tag.h`, for the C declarations, `performance-enum-size` five times, `modernize-use-using` twice and `readability-identifier-naming` twice; the magic numbers of the UTF-8 tables in `utf8.cpp` and `utf8_fuzz.cpp`, and the name of the libFuzzer entry point there; `cppcoreguidelines-avoid-non-const-global-variables` three times in the probe; in `syscall_trace.cpp`, `cppcoreguidelines-pro-type-vararg` four times for `ptrace`, `cppcoreguidelines-pro-type-union-access` once and `modernize-deprecated-headers` once for `<signal.h>`; `modernize-deprecated-headers` once in the writer; `concurrency-mt-unsafe` once for `getenv` in `tag_access_integration_test.cpp`.

## D-30: Review of tag read and write by three reviewers: contract 0.4, errors that leave no good value, the one-thread check
Date: 2026-11-14
Phase / issue: 1, #14
Status: accepted

Context: the pull request of issue #14 was reviewed by a C++, a SCADA architecture and a Linux systems reviewer, who then answered each other's findings. They confirmed the four choices D-29 asked about, two of them on condition that the product owner approves, and found defects. The product owner approved all four choices, asked for the contract to be changed now and for the one-thread check of contract 13.2 in the same pull request.
Options, the wording of contract 10.1 and 6.2 about the `down` flag: left for issue P1-13; changed now with a version bump. Options, the output of a read that fails for good: left unchanged, as for `SCADA_E_BUSY`; quality BAD. Options, the one-thread check: a later issue; this pull request. Options, the pause on ARM64: `yield` as now; `isb`.
Decision:
- Contract 0.4. Section 10.1 defines down by three rules and says the `down` flag of `scada-tagd` is for reporting only; section 6.2 holds the table of what a read returns. Layout version 1 is unchanged; the migration note is in the contract.
- Only `SCADA_E_BUSY` leaves the output of a read unchanged. With `SCADA_E_LAYOUT` and `SCADA_E_INVALID` the output gets value 0, times 0 and quality BAD, in `scada_tag_read_many` for each tag, and `scada_tag_read_string` gives length 0 and BAD in `meta`. A control task that keeps "the previous value" cannot then keep a good one for ever.
- OWNER_DOWN is never stored: a write with it is `SCADA_E_INVALID`, and a record that holds it is `SCADA_E_LAYOUT`.
- The one-thread check (contract 13.2): in a build without `NDEBUG`, the first thread that writes a tag of an owner becomes its thread, and a write from another thread stops the process. A thread is told by the address of a thread-local object, so the check makes no system call. `write_many` stops the process instead of skipping a handle that changed under it.
- The exception for `scada::cpu_relax()` is written into `CLAUDE.md` and section 11 of the coding standard.
- Conformance test 22 now also fails when the probed thread takes a page fault between the markers, and when the library reports that the clocks need a system call; it no longer excuses clock reads. It counts calls of the C++ allocation functions, not of `malloc`, which the banned-function check keeps out of the library.
- `tsc-early` is a clock source the vDSO reads. The ThreadSanitizer build does not compile for ARM64.
- Documented, not changed: a process that reads tags of other processes calls `scada_tag_status` about once a second until `scada_tag_notify` exists, or a late owner stays down for it; a tag that was not resolved in the context reads as OWNER_DOWN; where value and times are 0 there is no value.
- The pause on ARM64 stays `yield` for now. Measured on x86-64: 64 attempts with `pause` take about 1 microsecond, without a pause about 20 ns, and 250 byte stores of a string write take 110 ns to 150 ns. On ARM64, where `yield` does almost nothing, a string read may therefore give `SCADA_E_BUSY` while its owner runs normally. Whether to use `isb` is decided by measurement on ARM64.
Reason: an error that stays must not look like a value that is still good; that is the direction in which a control system fails safe. The contract disagreed with itself about the flag, and fail-safe behaviour is not settled in a decisions entry alone.
Consequences:
- Follow-up issues: #39, the model check of the sequence lock and the ring (conformance test 4), before phase 1 is accepted; #40, the retry window, `isb` and the rate of `SCADA_E_BUSY` on ARM64, with issue P1-12; #41, the clock check on real boards (an ARM timer erratum keeps the name `arch_sys_counter` while reads become system calls; compare with a raw system call), and an upper bound for a heartbeat time in the future; #42, smaller points of the review (a late owner found only through `scada_tag_status`, an error code for a tag that was not resolved, the length of a string without a buffer, a record of heartbeat gaps that `scada-tagd` does not see).
- For issue P1-07: consumers of a change ring get no entry when an owner dies, so they need the process status; the write path changes, so the real-time test and the latency figures are taken again.
- Reference section 5.4, which makes the `down` flag the source, is updated with the contract at 1.0 (contract section 17).
- New suppressions: in `tag_access_integration_test.cpp`, `concurrency-mt-unsafe` a second time for `getenv` and `modernize-deprecated-headers` for `<signal.h>`; `cppcoreguidelines-pro-type-union-access` once in the probe, for the page fault counts of `rusage`.

## D-31: The pause of a tag read on ARM64 is `isb`; no own attempt count and no shorter copy for strings
Date: 2026-11-14
Phase / issue: 1, #40
Status: accepted

Context: D-30 left `yield` as the pause of `scada::cpu_relax()` on ARM64 and asked for measurements on ARM64 hardware: whether 64 attempts of a read last long enough for an owner to finish a write, and the rate of `SCADA_E_BUSY`. The issue asks to decide from the figures whether ARM64 uses `isb`, whether the string functions get their own attempt count, and whether the string copy window is shortened. It forbids clock reads in the retry loop and a change of the limit of 64 for the real-time read before issue P1-12. The session that did the work has no ARM64 machine; the product owner chose the ARM64 runner of the pipeline.
Options, where to measure: the ARM64 runner of the pipeline; the target board, by hand; an emulator (gives no timing). Options, the pause on ARM64: `yield`; `isb`; no instruction. Options, strings: as now; an attempt count of their own; copy only `len` bytes or 64-bit words.
Decision:
- Measured on the pipeline runners with `scripts/measure-seqlock.sh` and the program `scada-tag-seqlock-bench`, in the `perf` build, by the manual job `arm64-measurements`. The figures are in `docs/reports/phase-1.md`.
- ARM64 uses `isb`. With `yield` the 64 attempts took 22 ns on a Neoverse N2, a fifth of one string write (113 ns), and 1 read in 80,625 of 1,000 string tags at 50,000 writes per second gave up. With `isb` they took 0.67 us and 1 read in 54 million gave up, against 1 in 25 million on the x86-64 runner with `pause`. A read of a record nobody writes takes the same time with both (15 ns to 17 ns); only a read that finds a write in progress waits longer, at most 0.67 us before it gives up.
- The build option `SCADA_ARM64_PAUSE` (`isb` or `yield`, default `isb`) is kept, so that a new board can be measured both ways. It is a definition of the whole build. x86-64 ignores it.
- The string functions keep the 64 attempts of the scalar read, and the copy stays as it is: with `isb` the window covers a string write about six times over, and the rate for string tags is below the criterion of the scalar ones.
- The limit of 64 is not changed; P1-12 confirms it. There are no clock reads in the retry loop.
Reason: 64 attempts must outlast the longest write a running owner makes, or a reader reports `SCADA_E_BUSY` while nothing is wrong. `isb` gives ARM64 the window x86-64 has with `pause`, without a clock read and without a second attempt count to keep consistent.
Consequences:
- The reference load (conformance test 2, 600 s, 4 readers, 50,000 tags, 50,000 updates per second) gave `SCADA_E_BUSY` for 0 of 33.1 billion reads with `yield` and 5 of 32.5 billion with `isb` on ARM64, and 0 of 26.1 billion on x86-64: all far below 1 in 1,000,000. On 2 processors 4 readers, 10 owner threads and `scada-tagd` share the cores, so a read that gives up there is an owner preempted mid-write, which no pause covers.
- One string tag rewritten without pause (one write every 195 ns) gives up for almost every read on both architectures: a 103 ns copy does not fit between writes. That is not a load of the reference document and is not addressed here.
- The runners are shared virtual machines with 2 processors, not the target board. The measurement is repeated on the board with the same script when it is available; issue #41 also needs the board.
- The job `arm64-measurements` runs only when started by hand (`workflow_dispatch`). It is not one of the checks of a pull request.

## D-32: Change rings and the wake mechanism: futex on read-only mappings, subscriptions, notify and wait sets
Date: 2026-11-14
Phase / issue: 1, #15
Status: accepted

Context: issue #15 (P1-07) adds the change entry of a write, `scada_sub_open`, `scada_sub_next` and `scada_sub_close`, gap detection and resynchronisation, and the experiment that settles the wake mechanism of contract 8.4 (open point of section 16, pass criterion conformance test 24). The acceptance names "conformance test 9", the number of contract 0.1; it is test 13 of contract 0.4. Test 24 needs `scada_tag_notify`, wait sets over several rings, the pollable descriptor and `cycle_seq`, and no other issue lists them, so they are part of this one; `scada_waitset_add_queue` comes with the queues (P1-08). The contract leaves open: what `scada_change` and `scada_sub_stats_t` hold, where a subscription starts, what a ring that is missing, appears or is replaced means for a subscription, what a consumer does with an entry that is not valid, how `scada_tag_notify` knows which rings the calling thread wrote, how many sources a wait set takes, and what makes a waiter on `cycle_seq` ready. Issue #42 asks to decide here whether `scada_tag_notify` takes over the look for missing regions (point 1) and what a ring consumer sees when an owner dies (point 2).
Options, mechanism: shared futex on the read-only mapping (the proposal); one eventfd per ring passed by `scada-tagd` over a Unix socket (the fallback); polling. Options, which rings notify wakes: every ring the process owns; the rings whose first writer is the calling thread. Options, a ring that appears or is replaced: deliver on silently; a gap. Options, an invalid entry: stop at it; skip it with `SCADA_E_LAYOUT`. Options, the waiter thread of a descriptor: one per process; one per wait set that asks for a descriptor. Options, the wake-up latency on ARM64: not measured; measured by the pipeline's native ARM64 runner.
Decision:
- Experiment and mechanism. A wait with `FUTEX_WAIT_BITSET` and no `FUTEX_PRIVATE_FLAG` on a 32-bit word of a read-only `MAP_SHARED` mapping of a region file is woken by `FUTEX_WAKE` on the same word through a read-write mapping of the file at another address, in one process (`Futex.*`) and between processes (`scada-tag-change-ring-integration-test`); `futex_waitv` on several such words likewise. The kernel keys a shared futex by file and offset. A wait compares the word first, so a wake before the wait is not lost (`Futex.AWaitOnAWordThatChangedReturnsAtOnce`). Results are below. The mechanism of contract 8.4 is kept; the eventfd fallback is not built.
- The word is the low 32 bits of `write_seq` (the targets are little-endian, so it is at the address of the field; `static_assert` in `src/futex.h`) and `cycle_seq`. A consumer waits with the low 32 bits of its cursor as expected value and an absolute `CLOCK_MONOTONIC` deadline; the owner wakes all waiters. Every wait is cut into slices of at most 100 ms, after each of which the ring is looked at again (contract 8.4). A timeout is from 0 ms; there is no wait without one.
- The change entry is appended in `write`, `write_many` and `write_string` when `write_record` reports a change, after `seq` is even again (contract 6.1), with the stored fields. The append uses the orders and fence of contract 8.1, field by field through `std::atomic_ref` as the sequence lock does (D-29). It is real-time safe; conformance test 22 covers it, since the probe writes a tag.
- `scada_tag_notify`: the first thread that writes the tags of an owner is now recorded in every build, not only in the debug check of contract 13.2 (a relaxed load per write; a compare-and-swap once). Notify wakes each ring whose recorded writer is the calling thread and whose `write_seq` changed since its last wake (one `FUTEX_WAKE` each), increments `cycle_seq` with an atomic add (several threads may notify) and always wakes its waiters, then looks again for missing regions and repeats the clock check, as `scada_tag_status` does. This settles point 1 of issue #42.
- `scada_change` (32 bytes): handle, type, quality, flags, value, source time, `update_mono_ns`, the stored fields (with the overlay of contract 6.2 while the owner is down, D-33). `scada_sub_stats_t` (40 bytes; 48 since D-33): changes received, gaps, entries lost to overruns, entries refused, epoch, owner and process index.
- A subscription starts at the end of the ring. `SCADA_E_GAP` is returned once for each of: an overrun (contract 8.2, `w - cursor >= capacity`, so a consumer may fall `capacity - 1` entries behind), a changed epoch, and a ring that appears, disappears or is mapped anew (a file replaced). Opening a subscription to a missing ring succeeds. The epoch is read before `write_seq`, so a restart between the two loads gives one gap too many and never one too few. After a gap the cursor is the current `write_seq` (contract 8.3).
- An entry that is not a valid change of a tag of the owner (handle, type, quality, flags, BOOL 0 or 1, STRING slot number) is counted and skipped with `SCADA_E_LAYOUT`. A ring header with another capacity than the owner table gives `SCADA_E_LAYOUT` at open and at every `scada_sub_next`.
- Wait sets hold up to 127 sources (the kernel's 128 words for one wait, less the stop word of the thread). A subscription is in at most one wait set; closing it takes it out, and destroying the set lets go of its subscriptions. A cycle source is ready when `cycle_seq` differs from the count the previous return of `scada_waitset_wait` saw.
- `scada_waitset_fd` starts one thread for that wait set (contract 8.4 says "one waiter thread in that process"; a process with one wait set has one). The thread waits with `futex_waitv` on the sources and on a private stop word, compares them with what it saw last (mapping, epoch, word) and writes to a non-blocking, close-on-exec eventfd when any changed. Its first view is taken before the look at what is ready already, which is signalled at once, so no change between the two is lost. Sources cannot be added once it runs.
- Point 2 of issue #42, an owner that dies: settled by the product owner, see D-33.
- The wake-up latency is measured by `scada-tag-wake-perf-test`: a writer process writes one tag every 2 ms with notify, and this process, blocked in `scada_sub_next`, takes `CLOCK_MONOTONIC` on return minus the entry's `update_mono_ns`, over 2,500 changes. It fails on x86-64 at a 99th percentile of 1 ms or more. Exception to D-13: both `build and test` jobs run it on the `release` build with `SCADA_PERF_REPORT_ONLY=1`, which prints the figures and gates on nothing, because the ARM64 runner is the only ARM64 machine the project has.
Reason: the futex needs no help from `scada-tagd` and passes nothing between processes, which keeps `scada-tagd` out of the data path (contract 17, item 3). Waking only the rings of the calling thread keeps the cost of notify proportional to what that thread wrote. A ring that appears or is replaced may hold changes the consumer never saw, so it must resynchronise, as after an overrun. Skipping an invalid entry keeps one damaged entry from stopping a consumer for good, while the return code still tells it.
Consequences:
- Results, x86-64 (container, Linux 6.18, 4 processors, Xeon 2.1 GHz): all `Futex.*`, `Changes.*` and `ChangeRingProcesses.*` tests pass in the dev, release, asan-ubsan and tsan builds. Wake-up latency between two processes, `perf` preset, 2,500 changes, two runs: median 61 µs both; 99th percentile 152 µs and 173 µs (target: below 1 ms); maximum 2.4 ms and 10.6 ms. The dev build gave median 56 µs, 99th percentile 149 µs. The maximum is a scheduling delay of the machine, which the 99th percentile target allows for.
- Results, ARM64 (the pipeline's native runner `ubuntu-24.04-arm`, 2 processors, run 43618327669 of pull request #45): the same tests pass in the `dev` and `release` builds. Wake-up latency, `release` build, 2,500 changes: median 8 µs, 99th percentile 17 µs, maximum 690 µs. The step "Wake-up latency, reported only (release)" of `build and test (ARM64)` prints it on every run.
- Contract 0.5: section 8 (when an entry is appended, entries are input, gap causes, epoch before `write_seq`, start at the end), 8.4 (the mechanism specified), 13 (`scada_change`, `scada_sub_stats_t`, timeouts), 16 (settled). Layout version 1 is unchanged.
- Issue #39 (model check, conformance test 4) covers the ring of `src/change_ring.cpp`.
- Not done here: `scada_waitset_add_queue` and the wake of queues (P1-08); `scada_proc_resolve` and `scada_proc_status`, which a consumer of `cycle_seq` would use to find a process by name; until then `scada_tag_info` gives the process index of a tag's owner.
- `scada_sub_stats_t` is 48 bytes since D-33.
- New suppressions: in `futex.cpp`, `cppcoreguidelines-pro-type-vararg` three times for `syscall` and `scada-banned,cppcoreguidelines-pro-type-reinterpret-cast` once for the address `futex_waitv` takes as an integer; in `scada_tag.h`, for the new C declarations, `modernize-use-using,readability-identifier-naming` twice, `modernize-use-using` twice, `readability-identifier-naming` twice and `performance-enum-size` once; in `c_api.cpp`, `readability-identifier-naming` twice for `scada_sub` and `scada_waitset`; in `change_ring_test.cpp`, `cppcoreguidelines-pro-type-vararg` once for `open`; in `subscription_test.cpp`, `bugprone-unchecked-optional-access` once after an assertion the check does not see; in `tag_wake_perf_test.cpp`, `modernize-deprecated-headers` once for `<time.h>` and `concurrency-mt-unsafe` once for `getenv`.

## D-33: A change ring reports its owner going down and coming back
Date: 2026-11-14
Phase / issue: 1, #15 and #42 (point 2)
Status: accepted

Context: a dead owner writes no change entry. A consumer that only follows the ring (alarms, historian, WebSocket feed) then keeps the last good value, which is the wrong direction for a control system to fail in. D-32 documented this and left the choice to the product owner in issue #42; the product owner asked for the gap.
Options: documentation only (D-32); a gap when the owner's process goes down and when it comes back; a special change entry written by `scada-tagd`, which would make it a writer of rings it does not own.
Decision:
- A subscription evaluates the owner's process by the three rules of contract 10.1 at every look, as the read overlay does, and returns `SCADA_E_GAP` once when the result changes from not down to down and once when it changes back. The cursor and the epoch then move to the end of the ring: the reads after the gap supersede what the ring held. The state at `scada_sub_open` is the starting point, not a change.
- While the owner's process is down, an entry that still arrives is delivered with the overlay of contract 6.2: quality BAD, OWNER_DOWN added. An owner whose heartbeat stopped can have threads that still write; their values must not reach a consumer as good.
- A restart gives two gaps to a consumer that looked while the owner was down, one to a consumer that did not. A ring that appears or is replaced at the same look as a change of the owner state gives one gap, not two.
- A wait looks again at least every 100 ms, and wait sets and their descriptor thread compare the owner state too, so a waiting consumer gets the gap within the owner's heartbeat timeout plus 100 ms, and at once after a clean stop.
- `scada_sub_stats_t` gets `owner_changes` and `owner_down` and grows from 40 to 48 bytes. Nothing had been released with 40 bytes; contract 0.5 says 48.
- `scada_sub_stats_t.last_gap` tells why the last gap came: `SCADA_GAP_OVERRUN`, `_RESTART`, `_RING`, `_OWNER_DOWN` or `_OWNER_BACK` (0 before the first). Since a gap now has two meanings (data lost; owner state changed), an alarm system needs to tell them apart without inferring it; the action stays the same, a read of the tags. When the owner's state changes at the same look as its ring appears or is replaced, the owner's reason is given, because that is what an alarm acts on. It takes one of the three reserved bytes; the size stays 48. Added after a review of this decision with the product owner.
Reason: a consumer of the ring learns of a dead owner from the ring, with the code it already has to handle, and the values it then reads are BAD with OWNER_DOWN. `scada-tagd` stays out of the data path.
Consequences:
- Conformance test 7 counts the second gap of a consumer that saw the owner down (contract 0.5, section 15).
- `scada_sub_next` reads the owner's `proc` region and the monotonic clock at every look: two loads and one clock read from the vDSO.
- Tests: `Changes.AnOwnerThatStopsGivesAGapAndAnotherWhenItIsBack`, `AChangeOfAnOwnerThatIsDownComesAsBad`, `AnOwnerDownAtOpenGivesNoGapUntilItComesUp`, `ABlockedConsumerLearnsThatTheOwnerStopped`, `AWaitSetWakesWhenTheOwnerStops`; across processes `ChangeRingProcesses.AKilledOwnerGivesAGapWithinItsHeartbeatTimeout`.

## D-34: The clock check compares with a raw system call; a heartbeat time in the future is bounded by the timeout
Date: 2026-11-14
Phase / issue: 1, #41
Status: accepted for the code; the run on the target ARM64 board is still open

Context: D-29 judged whether clock reads are system calls by the name of the clock source and a fixed limit of 200 ns for one read. The review of P1-06 (D-30) found that an ARM timer erratum workaround makes the vDSO fall back to the system call while the name stays `arch_sys_counter`, and that such a call can take under 200 ns on a fast core, while a slow core can take more than 200 ns in the vDSO. `proc_liveness` counted any heartbeat time ahead of the reader's clock as alive. The issue leaves open how much faster than the system call a read must be, and how far ahead a heartbeat time may lie.
Options, the time signal: a fixed limit, as before; a ratio to the time of a raw `clock_gettime` system call, timed on the same machine at the same moment. Options, the ratio: 1 (any speedup); 2; a larger factor. Options, the bound for a time in the future: a new constant; the heartbeat timeout of the process; a fraction of it.
Decision:
- `clock_syscall` is 1 when the clock source name is not one the vDSO reads, or when one library read of `CLOCK_MONOTONIC` takes more than half the time of one raw `syscall(SYS_clock_gettime, CLOCK_MONOTONIC)` (`CLOCK_READ_SPEEDUP` = 2). Both are the mean of the fastest of 4 batches; 256 reads per batch for the library, 64 for the system call. The fixed limit of 200 ns is gone. An empty name decides nothing, as before; a refused system call (a seccomp filter) leaves only the name to decide.
- A heartbeat time more than `heartbeat_timeout_ms` ahead of the reader's clock is down (`heartbeat stale`), in the library and in `scada-tagd` alike; exactly the timeout ahead is not. Contract 0.6 says so in section 10.1, and section 3.4 forbids a private time namespace.
Reason: a vDSO that falls back to the system call does the call and more, so it is never faster than the raw call, while a real vDSO read is several times faster on every core: the ratio holds on slow and fast cores where a fixed limit does not. A factor of 2 leaves room for noise in either direction. Measured on x86-64 here (virtual machine, `tsc`, 4 processors): library read 36 ns to 44 ns, raw system call 163 ns to 170 ns, a factor of about 4.5. A heartbeat can lie ahead of a reader's clock only by the time the reader spends between reading its clock and the heartbeat, which is microseconds; the timeout is the bound that needs no new setting, and a reader delayed longer than it sees the process down for a moment, which is the safe direction.
Consequences:
- The check takes about 0.1 ms, under 1 ms on the supported targets, at attach and once a minute, from `scada_tag_status` and `scada_tag_notify`. A real-time task that calls `scada_tag_notify` at the end of a cycle therefore spends about 0.1 ms in it once a minute, against a few tens of microseconds before.
- Not done in this pull request, so issue #41 stays open: running the check and conformance test 22 on the target ARM64 board, and on a machine booted with the vDSO clock disabled, and recording the figures here. The pipeline's ARM64 runner is a virtual machine. The unit test `ClockCheck.ASystemCallTimedAgainstItselfIsOne` stands in for the second: it times the raw system call against itself, which is what such a machine measures.
- In the ThreadSanitizer build the C library clock read is intercepted and takes about 100 ns against about 195 ns for the raw system call, at the limit, so the check can give 1 there and differ between two attachments. The unit test `TagAccess.TheStatusReportsTheClockCheck` therefore checks that one attachment reports the same result twice, not that two attachments agree. Conformance test 22, which expects 0, runs only in a native build without a sanitizer (D-29).
- New suppression: `cppcoreguidelines-pro-type-vararg` once in `clock_check.cpp`, for `syscall`.

## D-35: Smaller points of the review of P1-06: a tag that was not resolved, the length of a string, the fences of the ThreadSanitizer build
Date: 2026-11-14
Phase / issue: 1, #42
Status: accepted

Context: issue #42 collects six smaller points of the review of P1-06 (D-30). Each must be done or closed with a reason. Points 1 and 2 were settled with the change rings in P1-07. The product owner chose the options below for points 3, 5 and 6. Point 4 is not settled here.
Options, point 3, a tag of another process that was never resolved in the context: BAD with OWNER_DOWN as now; `SCADA_E_INVALID`. Options, point 5, `scada_tag_read_string` with `buf` NULL and `cap` 0: refused as now; the length alone. Options, point 6, the fences in the ThreadSanitizer build: left out, x86-64 only, as now; kept with `#pragma GCC diagnostic ignored "-Wtsan"`.
Decision:
- Point 1, a late owner found only through `scada_tag_status`: done in P1-07. `scada_tag_notify` looks again for missing regions and repeats the clock check (D-32), and the documentation of `scada_tag_status` and of the library says so.
- Point 2, ring consumers and a dead owner: done in P1-07. A subscription returns `SCADA_E_GAP` when the owner's process goes down and when it comes back, and an entry of an owner that is down comes with the overlay (D-33).
- Point 3: a read of a tag whose owner is another process, when no tag of that owner was resolved in the context, returns `SCADA_E_INVALID` with type 0, value 0, times 0 and quality BAD, as for a handle that is none. `scada_tag_read`, `scada_tag_read_many` and `scada_tag_read_string` do so. Resolve asks for the owner's `tags` region, and `PeerRegions` keeps an atomic flag for each slot, set with release by the first `want` and never cleared; the read loads it with acquire. One load, no lock, real-time safe. A tag resolved while its owner's regions are missing still reads as BAD with OWNER_DOWN. Contract 0.7, section 6.2.
- Point 5: `buf` NULL with `cap` 0 returns `SCADA_OK` with `*len` and `*meta` as a large enough buffer would give them. `buf` NULL with `cap` above 0 stays `SCADA_E_INVALID`, and a buffer that is not NULL but too small, `cap` 0 included, stays `SCADA_E_INVALID` with the length. Contract 0.7, section 13.
- Point 6: closed without a change. The pragma names one compiler's warning, and the project allows no compiler extensions outside the I/O HAL and the one exception of D-29. Keeping the fences would not make the ThreadSanitizer build check their order, because ThreadSanitizer does not model fences; the model check of issue #39 does. The ThreadSanitizer build stays on x86-64, where the fences are no instruction, and the `#error` for ARM64 stays.
Reason: a forgotten resolve that reads as a dead owner sends operators and alarm logic after a fault that is not there, while `SCADA_E_INVALID` names the mistake in the calling code, and quality BAD still keeps a caller that keeps its output from holding a good value. Asking for the length alone is the common C pattern and costs one comparison.
Consequences:
- Contract 0.7 with a migration note; layout version 1 is unchanged.
- A process that took handles from another process (they are the same in every process) must resolve one tag of each owner it reads before the first read, as contract 0.4 already required. The library's tests do so.
- Tests: `TagAccess.ATagThatWasNotResolvedHereIsInvalid` (replaces `ATagThatWasNotResolvedHereReadsAsOwnerDown`), `TagAccess.ATagResolvedBeforeItsOwnerStartedReadsAsOwnerDown`, and the length query in `CApi`.
- Point 4, heartbeat gaps that `scada-tagd` does not see: settled in D-36. `scada-tagd` sees only the heartbeat count and time at each check, so on its own it can prove a gap longer than the timeout only when exactly one heartbeat followed the gap before the next check; the owner's `scada_tag_heartbeat` knows each gap.

## D-36: A process records its late heartbeats; the heartbeat is safe from several threads
Date: 2026-11-15
Phase / issue: 1, #42 (point 4)
Status: accepted

Context: a reader judges liveness itself at every read (contract 10.1), while `scada-tagd` checks every `heartbeat_check_ms`. A heartbeat gap longer than the timeout and shorter than the check interval turns tags BAD with OWNER_DOWN and gives ring consumers a gap (D-33), and no "process down" record exists. Issue #42 asks to make such a transient traceable. D-35 left the choice to the product owner, who had it decided by a committee of a C++, a Linux systems and a SCADA architecture reviewer and accepted its recommendation. The committee was unanimous in both rounds. It also found that `scada_tag_heartbeat`, which contract 13.2 makes safe from any thread, loaded and stored the heartbeat counter and stored the time unconditionally: two threads could lose a count and publish an older time over a newer one, which shows a live process down to readers.
Options: A, `scada-tagd` infers gaps from the count and time it samples at each check; B, the owner counts its late heartbeats in reserved bytes of `ProcHeader` and `scada-tagd` logs them; C, close the point with a reason; a counter in `scada_proc_status` or a system tag (left for later). For the thread defect: fix it here; a separate bug issue first; narrow 13.2 to one thread.
Decision:
- B. `ProcHeader` takes `late_beats` (u32, offset 28), `longest_gap_ns` (i64, 32) and `last_late_mono_ns` (i64, 40) from its reserved bytes; 16 stay reserved; size and alignment stay 64. Contract 0.8, layout version 1.
- The writer (`OwnProcRegion::heartbeat`): the counter is a relaxed `fetch_add`; the time is advanced by a compare-and-swap (release on success) only when it is later than the published one, so it never goes back and a time that is not newer is not published. The thread that publishes owns the gap since the previous time: it raises `longest_gap_ns` (relaxed compare-and-swap), and for a gap strictly longer than the timeout stores `last_late_mono_ns` (relaxed) and increments `late_beats` (release). "Strictly" is the boundary of `proc_liveness`: a heartbeat exactly as old as the timeout is not stale, so a late beat is exactly a moment in which a reader could see the process down. Real-time safe: a few atomic operations, no system call; a retry only when another thread of the process published meanwhile.
- The first heartbeat of a life (`first_heartbeat`, in attach) stores the time unconditionally and is not measured: the gap since a previous life is a restart, and a time a previous life left ahead of the clock must not hold back this one. The figures of a reused region are kept, so `scada-tagd`, which keeps its mapping across a fast restart, never sees the count go back; a new region starts at 0.
- The timeout is the process's own `heartbeat_timeout_ms`, given to the region at attach. `scada-tagd` counts its own late heartbeats too: while they lasted, readers saw `SCADA_E_TAGD_DOWN`.
- The reader (`GenerationHost::check`): loads `late_beats` with acquire, then the gap fields, for every process including itself, and reports the growth since the previous check (modulo 2^32). The first sample of a mapping is a baseline: reported only if it is not 0, and marked. `scada-tagd` logs `heartbeat late` at WARNING (INFO at the first check of a new generation, as `process down`), also when a down or up change of the same process was logged in the same check, because only this record carries the gap; a baseline is logged once at INFO as `heartbeat late before this check`. At most one record per process and check.
- The fields are for diagnosis only and are never used to judge liveness; they are input to their readers.
- The thread defect is fixed in this change, because the same function is rewritten; contract 13.2 keeps "safe from any thread" and says that the published time never goes back.
- A is rejected: the writer stores the count before the time, so a sample can pair a count with the time of the beat before, and two clean gaps then look like one late one; and with several heartbeats between two checks, the normal case, it proves nothing.
Reason: only the owner sees every gap, and the comparison with its own previous time is cheap and exact. The record then matches what readers saw, so a BAD transient in the historian or an alarm can be traced to the process and the moment.
Consequences:
- Contract 0.8, sections 10, 10.1 and 13.2, with a migration note: a 0.7 library leaves zeros, which read as "none recorded".
- Tests that fail without the change: `LateHeartbeat.*` in `proc_region_test.cpp` (strict boundary, longest gap, a time that is not newer, the first heartbeat after attach, several threads), `Host.AHeartbeatGapBetweenTwoChecksIsReportedAsLate` and `Host.LateHeartbeatsFoundAtStartAreABaseline`, and `TagdService.ALateHeartbeatIsLogged` with real processes; `scada-tag-test-peer` takes `late=<ms>` for it.
- Follow-ups, not done here: the figures in `scada_proc_status` or a system tag (with `scada_proc_status`, which no issue has built yet, D-32); a system alarm or journal record for late heartbeats (alarm service); a count of fast restarts, which a reused region hides from `scada-tagd` (systemd's restart count and journal trace them meanwhile).
- The CAS is a load-linked/store-conditional loop on ARMv8.0 without LSE: lock-free, not wait-free; only threads of the same process contend.

## D-37: Conformance test 4: herd7 litmus tests of the sequence lock and the change ring, and a check that the ARM64 code matches them
Date: 2026-11-14
Phase / issue: 1, #39
Status: proposed (the allow-list change needs review by a person, D-21)

Context: conformance test 4 asks for a model check of the sequence lock and the change ring for the ARM memory model, "as written in sections 6 and 8". Issue #39 asks for a model of the protocols as `src/tag_record.cpp` and `src/change_ring.cpp` implement them, a check that each fence is needed, a run in the pipeline if the tool can be installed there, and the tool and its licence recorded. It leaves open the tool, how the model is tied to the code, and how the test is run. GenMC is GPL-3.0, which CLAUDE.md does not allow. herdtools7 is CeCILL-B, which is not on the allow-list.
Options, tool: herd7 (axiomatic, the Armv8 model `aarch64.cat` maintained by Arm, CeCILL-B); GenMC (checks C++ directly; GPL-3.0); Dartagnan (needs Java). Options, what is modelled: the contract's pseudocode; the instructions the code compiles to. Options, tying model and code: none, by review; a check of the disassembly of the ARM64 build. Options, how to get herd7 on the runner: opam; built from the source tag with the distribution's OCaml packages.
Decision:
- herd7 of herdtools7 7.58 (commit `1ca343e16a2038e406d1ac674e7e3a1b722b36c7`) with `aarch64.cat`. `CECILL-B` is added to the allow-list, for dependencies of scope `build` and `test` only, not for anything linked or shipped: a permissive licence (BSD-like, with an obligation to credit the authors), not copyleft. herdtools7 is listed with scope `test`; it runs on the build machine, is not linked and not shipped. The dependency check compares the licence without the scope, so the limit holds by review until the check enforces it (follow-up issue). A committee review of pull request #46 relayed by another session recommended this limit; the allow-list change still needs the review by a person of D-21. `aarch64.cat` is BSD-3-Clause.
- The litmus tests `tests/model/seqlock.litmus` and `change-ring.litmus` are written in the instructions the two files compile to with GCC and Clang at -O2: `fence_release` is `DMB ISH`, `fence_acquire` is `DMB ISHLD`, release stores `STLR`, acquire loads `LDAR`. The seqlock test is two string writes (`seq`, the slot, the record fields, `seq`) against one read attempt; the ring test is three appends to a ring of capacity 2, the third overwriting slot 0, against one read at cursor 0. Two locations stand for the fields of a record and its slot (a mixed copy of more fields contains a mixed pair), two for an entry. The owner's relaxed loads of its own `seq` and `write_seq` are written as the constants they give, since it is their only writer: with them, herd7 had 3 million candidate executions for the first version of the seqlock test and did not finish in 120 s on the runner. The condition is a copy the library would accept that is not one write whole.
- `tests/model/model_check.py` expects "Never" for each test and "Sometimes" for each of four copies with one ordering taken away: `DMB ISH` and `DMB ISHLD` replaced by `NOP`, `STLR` by `STR`, `LDAR` by `LDR`. The issue asks this for the two fences; the two other orderings are part of the same protocol and cost nothing to check.
- `tests/model/code_shape.py` disassembles the six protocol functions from the ARM64 library (`objdump -d -r -C`) and checks, on every path from entry to return with each branch taken at most twice, the order of barriers, acquire and release accesses and other loads and stores against the litmus tests, with a minimum number of loads or stores between them for the copy (compilers do not merge atomic accesses). Calls of functions of the same object file are followed: GCC 13 at -O2 does not inline `copy_fields`, `store_fields`, `changes_record` or `ring_header`. Paths that end in `panic`, `std::__glibcxx_assert_fail` or `__stack_chk_fail` are left out: they do not return and publish nothing; GCC repeats the start of a string write on such a path. It runs as `scada-model-code-shape-test` (label `unit`) in ARM64 builds with optimisation and without a sanitizer, so in the `release` tests of `build and test (ARM64)`. `scada-model-code-shape-self-test` runs it on a fixture of Clang 18 output on every host, as compiled and with each ordering edited out of each function.
- The model check is the test `scada-model-check`, label `model`, test preset `model` (no tests found is an error). It is registered when herd7 is found; `-DSCADA_REQUIRE_HERD7=ON` stops the configuration without it. The pipeline job `model check` builds herd7 from the tag in `setup-build-machine` (input `herd7`, the only place that names a package manager, D-13) and runs `ctest --preset model`.
Reason: `aarch64.cat` is written for herd7, and Arm validates its architecture model with it. GenMC's licence is not allowed; Dartagnan would bring a Java runtime to the build machine for the same model. A model of the instructions is the one that says something about ARM64 units; a model of C++ would leave the compiler's mapping unchecked. Without the disassembly check an edit that drops a fence from the C++ would leave the model passing, which is the failure the issue was opened for.
Consequences:
- Results (run 43634735302 of pull request #46, `ubuntu-24.04`, herd7 7.58, `aarch64.cat`): both tests as written "Never"; all eight weakened copies "Sometimes". One run of the seqlock test takes 44 s to 48 s, of the ring test 8 s to 10 s; the test takes 275 s. `scada-model-code-shape-test` passes on the GCC 13.3 `release` build of the ARM64 runner. In the session that wrote the check, Clang 18 output of 16 edits of the C++ (each fence dropped or moved, the end store or the first load made relaxed, a store after the end of a write) each failed it; that was not repeated with GCC.
- A change of the order of accesses in `tag_record.cpp` or `change_ring.cpp` changes the litmus tests in the same pull request (`tests/model/README.md`).
- Not seen by the disassembly check: which register holds which field. One copy load moved after `DMB ISHLD` while enough other loads stay before it passes, and a load after the fence through a register the copy used only once (the slot base in a string read) is not counted. Review covers that. The reader rule counts as part of the copy the bases loaded from at least twice, so that a private variable loaded once, the ring cursor, can be loaded again after the fence.
- Strengthening an ordering also fails the check (an acquire second load, `DMB ISH` in a reader, a second fence): the change needs the litmus tests and the check updated with it, which is intended.
- The model is bounded: two writes against one read attempt, three appends against one read. That the protocol holds for any number is the usual argument for a sequence lock, not a result of the tool.
- Outside the model and the disassembly check: the repair of contract 6.3 (`repair_tag_records` in `src/attachment.cpp`, which makes an odd `seq` even with a release store after writing BAD and NOT_INIT) and the store of the ring's `epoch` at reattach (`src/attachment.cpp`). Both run before the owner sets itself running; a follow-up issue adds them to the checks or records why not.
- The pipeline has seven checks; `model check` is added to the required checks of `main` (a repository setting, README.md, "Pipeline").
- D-21: the allow-list holds `CECILL-B` too.
