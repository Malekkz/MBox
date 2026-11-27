# libscada-driver

The driver framework that every protocol driver and `scada-io` are built on
(issue #104, P2-07). The behaviour is that of the driver interface contract,
`docs/contracts/driver-interface.md` (version 0.6); the configuration keys
are those of the configuration schemas contract (version 0.6).

The library is built in slices. These are the first two:

| Part | Header | Contract |
| --- | --- | --- |
| Raw values, as a protocol's decode delivers them | `scada/driver/raw_value.h` | 5.1, 5.2 |
| Compiled rules of a point's common keys, and their check | `scada/driver/point_rules.h` | 5.1 to 5.8; CS 4.7, 4.9, 5.3 to 5.5 |
| Processing after decode, staleness | `scada/driver/sample_processing.h` | 5.1 to 5.8, 7.5 |
| Device local time to UTC, from the unit's zone files | `scada/driver/zone_table.h` | 5.8; CS 4.6 (decision D-130) |
| Engineering value to raw value for a write; raw patterns | `scada/driver/write_conversion.h` | 8.4; CS 7.2 |
| The frame protocol interface and the capabilities | `scada/driver/protocol.h` | 2.2, 2.3, 2.8 |
| The run-time objects of an exchange: connection state, attempt, sample sink | `scada/driver/exchange.h` | 2.2, 3.1 |
| The compiled channel, device, point and poll group | `scada/driver/compiled.h` | 3.1, 6.1, 6.4, 6.5 |
| Block planning of reads | `scada/driver/plan.h` | 6.3, 6.5, 6.6, 5.5 |
| The order of service, pacing, keep-alive, retries, probes | `scada/driver/scheduler.h` | 6.2 to 6.6, 7.1, 7.4 |
| The deadlines of an exchange | `scada/driver/timing.h` | 7.1, 7.2 |
| Device states, demotion, probes' back-off, the run of time-outs | `scada/driver/failure.h` | 3.5, 7.3, 7.4 |
| What the engine needs of a connection and of time | `scada/driver/transport.h` | 7.1 to 7.3 |
| The exchanges of a channel worker: connect, retries, demotion, probes, closes, staleness | `scada/driver/channel_engine.h` | 6.2, 7.1 to 7.5 |
| The real connections: TCP, a serial port, the system clocks | `scada/driver/connections.h` | 7.1 to 7.3; CS A.1 (decision D-141) |
| The driver process: main thread, a worker per channel, the writes after attach, the channel watchdog, the bounded stop | `scada/driver/driver.h` | 2.3, 2.4, 2.6, 9.1, 10 (decision D-142) |

Not yet here: the system tags, identity and restarts, and the workers' log
queues (slice 5b),
commands, the generation change and reload. `docs/decisions.md` D-130 to
D-142 record the choices the contracts left open.

## What it does

- `process_sample` takes one raw sample of a point (an outcome, the raw
  pattern and number), the point's rules and run-time state, and the
  samples of its companions in the same poll with the quality their own
  processing gave them. It applies, in order: invalid markers on the
  pattern, non-finite values, the exponent, the scale, counters, double
  points, enumerations and bits, the quality and health rules (for every
  outcome), and the time of the sample. It fills a result the caller keeps
  (the point's tag and its bit tags). It allocates nothing, takes no lock
  and reads no clock (`SCADA_RT`).
- `check_stale` adds STALE once to a point that no read confirmed within
  its `max_age`, and turns a held value (`keep_last`, DEVICE_INVALID) BAD
  after `max_age`.
- `to_raw_value` converts a write's engineering value to the raw value, on
  the exact integer path or in binary64, and returns the engineering value
  the raw value reads back as; it refuses what does not convert exactly as
  the contract states. `raw_from_pattern` gives the raw value of a
  configured raw pattern (a safe state or a release value).
- `validate_point_rules` states what the compile step must refuse; rules
  that reach the processing anyway give BAD, never undefined behaviour.
- The library is compiled with floating-point contraction off
  (`-ffp-contract=off`, decision D-98), so scaled values have the same bits
  on x86-64 and ARM64.

- `plan_blocks` groups a device's points into the requests of each poll
  group within the read rules and the limits of each space, with a split
  form per request and a keep-alive request (D-135). A protocol whose points
  are spans of numbered units can use it as its `plan`.
- `Scheduler::take` chooses a channel worker's next request at a given
  monotonic time: the next step of an exclusive order first, then orders,
  then due poll groups (a chain of companions in progress first, then by
  priority and due time), then a keep-alive, within the pacing of each
  physical unit and of the channel (D-136). Only the channel worker calls
  it. It does no I/O and allocates nothing; the caller passes the time, so
  every test is deterministic. A channel the scheduler cannot run is
  refused once, when it is built (`Scheduler::status`).

## Known behaviour

- **A list of writes holds its unit.** A list (8.5) is an exclusive
  sequence: 5 writes at a 10 s write interval hold the unit and the devices
  that share it for 40 s, with no poll and no keep-alive (D-136).
- **The contract's order of service; dead devices are a known limit.**
  Orders first, then a chain in progress, then priority, then due time; a
  retry takes its turn in that order (DI 6.2). Until a dead device is
  demoted, its attempts, each up to its time-out, take their turns like any
  due request and delay the live devices of its priority and below; a dead
  device of a faster group keeps the slower groups waiting. The figures are
  in D-138 (`ctest --preset perf`); serving answering devices first is
  #147. A device's state tag can show RUNNING for minutes after its points
  are COMM_FAIL: refuse a write on the points' COMM_FAIL, not on DEMOTED.
- **Probes use the windows between polls.** A probe goes when no request
  waits, its time-out ended before the next due poll if the window is at
  least 100 ms; one such probe per due time, and its time-out is no
  failure. Else, and after it, the probe waits up to 10 time-outs and then
  goes with its whole time-out (1 s at most) ahead of due polls, the oldest
  first, at most one such probe per channel in 11 probe time-outs: a
  returned unit is found also on a line where a request is always due, and
  once the dead are demoted a live unit waits at most its period and one
  probe time-out (2.03 s on the 250-unit line; D-138).
- **Dead values go bad on time.** A poll's points are COMM_FAIL at the
  latest one period (or its retries' time-outs, if longer) after its first
  time-out, however long its retries wait for their turn (D-140).
- **A dead block of a live device rests.** A request that keeps failing
  while its device answers others is polled once per back-off (10 s,
  doubling to 300 s, by default); its points stay BAD. A request of a chain
  of companions never rests: a dead part of a chain is attempted every
  period (D-140).
- **A line that is never quiet is jammed.** When quiet times keep finding
  bytes for 5 s, a serial line counts a jam (`jams`) and sends anyway: no
  request for at most 5 s plus one quiet time (D-140).
- **A device with no probe request is probed by a poll cycle.** When every
  read of a device has side effects or is chained, its probe is one whole
  poll cycle at each back-off instant, and its samples are published as a
  poll's: a side-effect read is made once per back-off (D-138).
- **The connect blocks the worker.** `step` waits in the transport's
  connect up to `connect_timeout` (3 s by default); orders that arrive
  meanwhile wait as long. The command slices make the connect polled
  (D-140).
- **Orders can starve a chain.** On a busy unit, orders may keep dropping a
  chain of companions, which then never completes; there is no cap, and the
  poll group's achieved period shows it (D-136, R7).

- **Counter wraps.** A decrease from at least `limit`/2 to below it is a
  wrap (contract 5.4): it is written GOOD with no flag, whatever
  `on_decrease` says. A 32-bit counter that steps back from 3,000,000,000 to
  2,000,000,000 is therefore read as a wrap. A 64-bit counter never comes
  near half its limit.
- **Setpoints are rounded to the raw step.** On the binary64 path with an
  integer raw type, 12.34 on `divide_by: 10` is written as raw 123, which
  reads 12.3; `to_raw_value` returns 12.3 as the value written.
- **A late poll group does not catch up.** When a cycle ends after the next
  one was due, the next starts at once, and missed cycles are not sent.
- **SUBSTITUTED** is not written until tag database contract 1.1 (#89): a
  configured substitute is UNCERTAIN, and incoming flags are masked.
- **No integer tag on a floating raw type** (CF-504, D-134), other than an
  enumeration or a double point: the rules are refused, a read is BAD with
  OUT_OF_RANGE and a write is refused. Use `value_type: float64`.

## Zone files

Device local times are converted with the unit's compiled zone files
(TZif): the directory named by the environment variable `TZDIR`, else the
one the CMake option `SCADA_ZONEINFO_DIR` sets (default
`/usr/share/zoneinfo`); either must be an absolute path. A target image must
carry them there. They are read when a configuration is compiled.

- A zone name is an IANA name; `localtime`, `posixrules` and names under
  `posix/` or `right/` are refused.
- The file is opened beneath the directory (`openat2` with
  `RESOLVE_BENEATH`, Linux 5.6 and later). A link inside the directory, as
  zone aliases are installed, is followed; a link that leads out of it, or
  an absolute link, is refused. Where the kernel has no `openat2` (ENOSYS)
  the path is resolved and checked in user space (D-130).
- Deployment requirement: a service sandbox (a seccomp filter, a container
  profile) must allow `openat2`. A filter that answers EPERM is a refusal
  and is not taken as a missing call, so the zone file is not read and a
  configuration with a local-time device is refused.
- A file without a footer rule makes every time after its last transition
  uncertain.
- A file in which more than 16 begins of intervals of constant offset
  (the table's start and each change) lie within less than the span of its
  offsets (the largest minus the smallest, with 0) of one another is
  refused as not valid, so that every conversion looks at a bounded number
  of intervals: at most 33 (#158, D-144).
  No real zone file comes near it: in each of the 499 zone files of the
  build machine, changes are further apart than the span of the offsets.

## Testing

```
cmake --build --preset dev --target scada-driver-unit-test scada-driver-zones-test \
  scada-driver-transport-test scada-driver-test-sim scada-drv-test \
  scada-driver-process-integration-test scada-tagd
ctest --preset unit -R 'scada-driver|sample_processing|tzif'
ctest --preset integration -R 'scada-driver-(zones|transport|process)'
```

To run a driver of the test protocol by hand against the simulator and a
running scada-tagd (its tag set must have the process `drv-test` and the
tags `<device>.<point>` of type INT64):

```
scada-driver-test-sim --tcp --map 1 0 0 3 --set 1 0 0 1234   # prints "tcp <port>"
scada-drv-test --base-dir <dir> --tagd-uid <uid> \
  --channel line1 tcp 127.0.0.1 <port> --device d1 1 --point v 0 1
scada-drv-test --help
```

`scada-drv-test` stops on SIGTERM or SIGINT with status 0 within the stop
budget of 2 s (a read in flight is given up), ends with 4 on a stall (2.6)
or a worker that did not stop within the budget, 1 when it cannot start or
the generation retires (the reason on standard error), 2 on a bad command
line, 3 when a protocol call ran on the wrong thread. SIGHUP is ignored
until reload; every other signal has its default.

- The golden vectors are in `tests/golden/driver-vectors.txt`, a data file
  that the protocols and their simulators share (its header gives the
  format); `tests/golden_vectors_test.cpp` runs them. They compare value
  bits, so a difference between the architectures fails them.
- `scada-driver-zones-test` compares every zone of the build machine's
  database with the C library's reading of the same files, by a full search
  around every change from 1970 to 2100 (about a million local times, some
  15 s).
- The unit tests of local times need the zones `Europe/Berlin`,
  `America/Vancouver`, `America/New_York`, `Australia/Sydney`,
  `Australia/Lord_Howe`, `Europe/Dublin` and `Etc/GMT-3`; a machine without
  them fails the tests, it does not skip them.
- Fuzz targets: `sample_processing_fuzz` (samples, rule fields, a local
  zone, the write conversion; every accepted write is read back through the
  processing and must give the value written) and `tzif_fuzz` (the zone file
  reader and the TZ string parser; transitions ordered, offsets within
  RFC 9636's bounds, an instant converts back to itself).
- `zone_file_test.cpp` builds a zone directory in the temporary directory,
  with links inside and out of it, a FIFO and a subdirectory, and reads it
  with `openat2` and with the fallback.
- The conformance tests of scheduling (9 to 14, the parts that need no
  failure handling) run the test protocol (`tests/test_protocol.h`, a
  scripted frame protocol with invented frames) over an in-process loopback
  (`tests/loopback.h`): simulated devices that answer its frames and log
  every request with its time, and a loop that drives the scheduler on an
  injected clock. Nothing sleeps; each exchange takes 1 ms of simulated
  time. `plan_test.cpp` checks the planner against plans worked out by
  hand, and `plan_property_test.cpp` checks every rule on 10,000 random
  plans from a fixed seed.
- The conformance tests of failure handling (15, 16, 17, the dead-device
  part of 12 and the gateway and busy parts of 19) run the channel engine
  over a simulated transport (`tests/sim_transport.h`) on the same bus:
  answers arrive after a fixed latency on an injected clock, and a unit can
  be silent, busy, unreachable behind its gateway or answer garbage;
  connects can be refused and the peer can close. `failure_test.cpp` and
  `timing_test.cpp` check the accounting and the deadlines alone.
- `scada-driver-transport-test` (integration) runs the real transports
  across the boundary. A channel engine reads a device through
  `scada-driver-test-sim`, the test protocol's simulator in its own process
  (`tests/test_sim_main.cpp`; contract 14), over a TCP socket on 127.0.0.1
  and over a pseudo-terminal pair; the simulator reports every request and
  answer with its monotonic time, so the test checks the silence before
  each serial request. The rest runs against peers the test holds: a
  refused and a closed connection, a connect that times out, a send after
  a reset, a partial send, the abortive close, an interrupted wait, the
  socket options, a port opened twice, a rate and a parity the port does
  not take, an inherited mark or space parity, bytes queued before the
  connect, paths that are not ports. The self-connection check, the receive
  loop and the discard limit are unit tests (`connections_test.cpp`). A pseudo-terminal has no bit rate or
  parity (7.2): line timing is tested on the target board (#41). Every wait
  is on a condition with a time-out; nothing sleeps.
- `scada-driver-process-integration-test` (in `tests/integration`) runs
  scada-tagd, the simulator and `scada-drv-test` as processes, with a
  listener standing in for the service manager, and reads the tags through
  the C interface (`driver_fixture.h`). `driver_integration_test.cpp`: the
  writes after attach (9.1) before READY, a restored tag, a disabled
  channel, a kill and a restart, the owners of the tags, one notify per
  change, every protocol call on its thread (test 1), a lost and a refused
  connection (test 16), STALE (test 17). `driver_stop_integration_test.cpp`:
  READY, WATCHDOG and STOPPING, a wrong notify socket, the signals, a stop
  with a read in flight, an idle worker, a peer's close, a stop and a
  retiring generation with a hung worker, a stall, a spinning worker, a
  pause of the whole process, and the start's exit statuses (test 2). It
  takes about 2 minutes; the idle case alone runs 20 s.
