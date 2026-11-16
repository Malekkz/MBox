# tests

Tests that span several components, the code tests share, and the samples
that prove the test tooling works. Decisions: `docs/decisions.md`, D-10 to D-12.

## Layout

| Path | Content | Label |
| --- | --- | --- |
| `<component>/tests/` | Unit tests of one library; run in seconds | `unit` |
| `<component>/fuzz/` | Fuzz targets of one library, seed corpus in `corpus/<target>/` | `fuzz`, `unit` |
| `tests/integration/` | Tests that start real processes | `integration` |
| `tests/install/` | Tests of the staged install tree and the systemd units | `integration` |
| `tests/perf/` | Performance tests against stated budgets | `perf` |
| `tests/model/` | Model check of the sequence lock and the change ring for the Armv8 memory model, and the check that the ARM64 code matches it | `model`, `unit` |
| `tests/support/` | `scada::test-support` (start a process, wait on a condition, trace the system calls of a process, a temporary directory a killed test does not leave behind) and its cleaner `scada-test-cleaner`, and the corpus replay driver | |
| `tests/sanitizer-fixtures/` | Deliberate bugs, one per sanitizer finding | `sanitizer` |
| `tests/fuzz-sample/` | Toy parser with a planted bug, and its fuzz target | `fuzz`, `unit` |

The commands for each kind are in the top-level `README.md`, "Testing".

## Adding a test

Unit, integration or performance test, written with GoogleTest; `main()` comes
from the framework:

```cmake
scada_add_test(scada-foo-unit-test KIND unit
  SOURCES foo_test.cpp
  DEPENDS scada::foo)
```

An integration test starts its processes with `scada::test::ChildProcess`
(`support/include/scada/test/child_process.h`) and waits with
`wait_for_output`, `wait_for_error` or `wait_for_exit`, each with a timeout.
Do not sleep. Pass the command to start through `ENVIRONMENT`, as
`integration/CMakeLists.txt`
does, so that a cross build can run it through the emulator. Start processes
from the main thread of the test: a child of `ChildProcess` is killed when
the thread that started it ends, which is how it dies with a killed test
binary (decision D-48). Make temporary directories with
`scada::test::TempDirectory` (`support/include/scada/test/temp_directory.h`),
whose cleaner removes the directory if the test binary is killed.

To show that code makes no system call, the program under test calls
`scada::test::mark_trace_begin()` and `mark_trace_end()` around it, and the
test runs the program with `scada::test::trace_marked_syscalls`
(`support/include/scada/test/syscall_trace.h`), which returns the system
calls made between the markers. `integration/rt_safety_integration_test.cpp`
does this for the real-time safe functions of the tag library. It works in
a native build without a sanitizer only; register such a test as that file
is registered.

### Load

The integration tests wait on conditions, but some check a time bound of
the contract or a heartbeat timeout, and these assume a machine that gives
each process the CPU within about a second. `ctest -j8` on 4 cores with a
busy loop on every core is within that: 11 of 11 full runs passed before
decision D-50, and the tests it fixed passed 200 of 200 loaded runs. With
eight more test binaries in parallel on the same 4 cores (load average 18
to 40) the machine stalls for 3 s and more, and the following fail now and
then without a fault in the code: the 2,000 ms limit for a killed owner to
read as down in `TagAccessProcesses.Conformance05A*`, the 95 % rate of
`Conformance02ReferenceLoadWithFourReaders`, and tests with a short
heartbeat timeout. The tag sets of the load tests use a heartbeat timeout
of 30 s (`LOAD_HEARTBEAT_TIMEOUT_MS`), so that such a stall does not make a
running owner read as down. `SimProcesses.AnOverloadedRunStopsPromptly`
keeps the default of 3,000 ms under load.

A test whose writer could finish before its readers start holds the writer
until they run: `scada-sim run --hold` writes only after SIGUSR1
(`tag_access_integration_test.cpp`, `run_load`).

Fuzz target: a source file that defines `LLVMFuzzerTestOneInput`, with at
least one seed input in `corpus/<name>/` next to it. Copy the entry point
from `fuzz-sample/fuzz/toy_parser_fuzz.cpp`; the suppression on it is needed
because libFuzzer fixes that name:

```cmake
scada_add_fuzz_target(foo_frame_fuzz
  SOURCES foo_frame_fuzz.cpp
  DEPENDS scada::foo)
```

In the `fuzz` build this is fuzzed for 60 s as test `foo_frame_fuzz`. In every
other build the seed corpus is replayed once as unit test
`foo_frame_fuzz-corpus`. When fuzzing finds a failing input, it is written to
`build/fuzz/.../<name>-work/artifacts/`; fix the bug and add that file to the
seed corpus.

## The samples that contain bugs

`sanitizer-fixtures/` (heap overflow, use after free, leak, signed overflow,
data race) and the toy parser in `fuzz-sample/` are wrong on purpose. Their
tests pass only when the tool reports the bug and the process fails. Do not
fix them, do not copy from them, and never link them into product code. The
fixtures are built only in the `asan-ubsan` and `tsan` builds and are not
covered by the `check` target.

## Passing the `check` target in a test

- After `ASSERT_TRUE(value.has_value())` the linter still reports the access
  to the optional. Write `if (!value.has_value()) { FAIL() << "why"; }`, or
  compare the optional itself: `EXPECT_EQ(child->wait_for_exit(limit), 0)`.
- `TEST` and `TEST_F` need no suppression for the `new` inside the macro.

## The install tests

`install/` holds three tests of the staged install tree (`docs/install.md`),
written as CMake scripts because they run tools, not C++:

- `install-stage` installs the build into `build/<preset>/tests/install/stage`.
- `install-layout` checks that tree against the directory layout of
  reference 4.4.
- `install-units-verify` runs `systemd-analyze verify` on every service unit
  in it. It needs `systemd-analyze` from systemd 250 or newer and fails
  without it.

```
ctest --preset dev -R '^install-'
```
