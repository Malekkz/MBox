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

## D-6: The staged tree is an image of the root filesystem
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

## D-7: The systemd notification protocol is implemented in `libscada-service`
Date: 2026-11-13
Phase / issue: 0, #6
Status: accepted

Context: a `Type=notify` service must send `READY=1` and `WATCHDOG=1` to the socket in `NOTIFY_SOCKET`. No dependency list or licence report exists yet (issue P0-07), and `libscada-common` does not exist yet (issue P0-06).
Options: link `libsystemd` (`sd_notify`); implement the documented datagram protocol; start services through a wrapper that notifies for them.
Decision:
- A new library, `libscada-service`, implements the client side of the protocol with plain sockets. It also holds `NotifyListener`, the service manager's side, which tests use in place of systemd.
- Values of `NOTIFY_SOCKET`, `WATCHDOG_USEC` and `WATCHDOG_PID` are validated as untrusted input. A service refuses to start on an invalid value instead of running without notification.
- Until P0-06: the library carries its own `Fd` owner type, and returns `NotifyStatus` and `std::optional` instead of `Status` and `Result<T>`. P0-06 moves `Fd` to `libscada-common` and converts the return types.
- `bind()` needs a `sockaddr*` for a `sockaddr_un`. The conversion goes through `void*` in one function, `bind_unix`, used only by `NotifyListener`. Sending uses `sendmsg()`, which takes the address as `void*` and needs no conversion.
- The environment parser has unit tests for invalid, oversized and overflowing values but no fuzz target, because the fuzzing set-up is part of issue P0-03. The fuzz target is added there.
Reason: the protocol is a few lines, is documented as stable by systemd, and avoids a dependency before the licence report exists. A wrapper would report readiness for a service that is not ready.
Consequences: only the messages the services need are supported (`READY`, `WATCHDOG`, `STOPPING`, `STATUS`); file descriptor passing and `sd_notify` over other socket types are not.

## D-8: Service unit template and the watchdog test hook
Date: 2026-11-13
Phase / issue: 0, #6
Status: accepted

Context: reference 4.2 fixes `Type=notify`, `WatchdogSec`, one user per service and three sandboxing options. It leaves the watchdog interval, the restart policy and the way to hang a service deliberately open.
Options, restart policy: `Restart=on-failure` with systemd's default rate limit; full backoff and restart limit of reference 14.4 now. Options, hang: a signal; a command-line option; a debug build only.
Decision:
- All units are generated from `deploy/systemd/scada-service.service.in` by `scada_install_service(<name> DESCRIPTION <text> WATCHDOG_SEC <seconds>)`. The watchdog interval is set per service; `scada-hello` uses 10 s. Services feed the watchdog at half the interval.
- The template sets `Restart=on-failure`, `RestartSec=1`, and leaves systemd's default rate limit (5 starts in 10 s) in place. Backoff, the restart limit and the system alarm of reference 14.4 are set when supervision is built.
- The template sets `ProtectSystem=strict`, `NoNewPrivileges=yes`, `PrivateTmp=yes` and empty `CapabilityBoundingSet=` and `AmbientCapabilities=`. A service that needs a writable directory or a capability adds it to its own unit when it is built. `NotifyAccess=main` accepts notifications from the main process only.
- `scada-hello` hangs its main loop on `SIGUSR1`. Only `scada-hello` has this hook; it is not part of the template or of `libscada-service`.
- The test `install-units-verify` runs `systemd-analyze verify --root=<staged tree> --recursive-errors=no`, which needs systemd 250 or newer on the build machine. The test fails if `systemd-analyze` is missing.
Reason: the template holds what every service shares; values that depend on the service stay with the service. A signal needs no change to the unit to trigger a hang on a test machine.
Consequences: every service unit starts from the strictest sandbox and widens it explicitly. Product services get no hang hook (reference 13.6: no debug interfaces in release builds).
