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
| `tests/perf/` | Performance tests against stated budgets; the baseline medians of the gate in `baseline/` | `perf` |
| `tests/conformance.md` | Where each conformance test of the tag database contract is automated; checked by `scada-conformance-index` | `unit` |
| `tests/model/` | Model check of the sequence lock and the change ring for the Armv8 memory model, and the check that the ARM64 code matches it | `model`, `unit` |
| `tests/support/` | `scada::test-support` (start a process, wait on a condition, trace the system calls of a process, a temporary directory a killed test does not leave behind, the processors a thread runs on) and its cleaner `scada-test-cleaner`, and the corpus replay driver | |
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

The suite is expected to pass on 4 cores with a busy loop on every core,
`ctest -j8` in a loop and up to eight more test binaries in parallel
(load average up to about 20; decision D-56, issue #73). Tests wait on
conditions with a limit (WAIT_LIMIT, 30 s), and a test process that waits
for long writes its heartbeat or has a heartbeat timeout of 30 s
(`LOAD_HEARTBEAT_TIMEOUT_MS`), so that a stall of the machine does not
make a running process read as down.

Some tests check a time that is the subject of the test, and assume a
machine that gives each process the CPU within about a second:

- the contract's bounds: the 2,000 ms for a killed owner to read as down
  in `TagAccessProcesses.Conformance05A*`, the 95 % rate of
  `Conformance02ReferenceLoadWithFourReaders`, the gap within the heartbeat
  timeout of `ChangeRingProcesses.AKilledOwnerGivesAGapWithinItsHeartbeatTimeout`;
- tests with a short heartbeat timeout of a running process, such as
  `TagdService.Conformance20RestartLeavesTheOtherProcessRunning` (1,000 ms);
  `SimProcesses.AnOverloadedRunStopsPromptly` keeps the default of 3,000 ms;
- the unit tests of the fixture `AttachFixture`, whose processes write a
  heartbeat only when a test asks and have the default timeout of
  3,000 ms.

Beyond the stated load (load average 18 to 40, stalls of 3 s and more)
these fail now and then without a fault in the code, and the ctest
limits of `scada-tag-unit-test` (120 s; 35 s unloaded) and
`scada-model-code-shape-self-test` (60 s; 10 s of CPU) can be exceeded. The
tag sets of the load tests use a heartbeat timeout of 30 s, so that such
a stall does not make a running owner read as down.

A test whose writer could finish before its readers start holds the writer
until they run: `scada-sim run --hold` writes only after SIGUSR1
(`tag_access_integration_test.cpp`, `run_load`).

### Processors and root

To put a process or a thread on chosen processors, pin the main thread of
the test with `scada::test::PinnedThread`
(`support/include/scada/test/cpu_affinity.h`) before it starts them: a
process and a thread inherit the set of the thread that starts them.
`TagAccessProcesses.Conformance03*` runs the writer on one processor and
the readers on the others. It skips with fewer than 2 processors, unless
`SCADA_REQUIRE_SEPARATE_CORES=1`, which the pipeline sets: then it fails.
It is the CTest test `scada-tag-separate-cores-integration-test` of its
own, whose SKIP_REGULAR_EXPRESSION makes a skip show as skipped;
`scada-tag-access-integration-test` leaves it out with GTEST_FILTER.

Tests that need root have the label `root` instead of `integration`, and
`sudo ctest --preset root` runs them (decision D-42):
`scada-tag-observer-users-test` becomes other users;
`scada-tag-full-shm-root-test` (conformance test 11) mounts a small tmpfs
in a mount namespace of its own and never touches `/dev/shm`;
`scada-tag-false-trip-root-test` (conformance test 26) runs the readers of
`scada-tag-rt-reader` under SCHED_FIFO beside the reference load. Its
criterion for `SCADA_E_BUSY` gates only in a Release or RelWithDebInfo
build without a sanitizer (`SCADA_TAG_BUSY_GATE=1`, set by CMake); the dev
build prints the rate (decision D-57). It runs 10 s, or
`SCADA_CONFORMANCE_SECONDS`.

### Performance gate

`ctest --preset perf` is the gate of the pipeline (decision D-57). A
performance test fails on a target, on every architecture; the pipeline
runs it in the step "Perf gate (perf)". `scada-tag-latency-perf-test`
(conformance test 27) also compares the median of each call with
`baseline/<processor>.txt` (lines `<call> <median ns>`, as the test prints
the calls) and fails above `SCADA_PERF_TOLERANCE` times it (2.0). A call the
baseline does not name is reported only. Change a baseline only in a pull
request that says why, with the figures of the runs it comes from.

A conformance test of the contract (section 15) is listed in
`conformance.md` with the tests that automate it. The unit test
`scada-conformance-index` fails if a test of section 15 has none, if a test
named there does not exist, or if a test named `Conformance<NN>...` is not in
row NN.

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
