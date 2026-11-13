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

## D-10: `Status` carries a code and a fixed message; `Result<T>` is our own type
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

## D-11: Logging through a bounded queue, to the journal's native socket
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

## D-12: Suppression comments are not wrapped by the formatter
Date: 2026-11-13
Phase / issue: 0, #7
Status: accepted

Context: a suppression is valid only on one line, with the check name and the reason (coding standard, section 14). With an 80-column limit clang-format wraps such a comment, and the wrapped `NOLINTNEXTLINE` then applies to its own second line instead of the code.
Options: keep every suppression under 80 columns; move the reason to a separate comment; tell clang-format not to wrap these comments.
Decision: `.clang-format` sets `CommentPragmas` so that comments starting with `NOLINT` are left as written. Such a line may exceed 80 columns.
Reason: several check names alone leave no room for a reason within 80 columns, and section 14 wants the reason on the same line.
Consequences: suppression lines are the only lines allowed to be longer than the column limit.
