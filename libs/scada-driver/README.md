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

Not yet here: the real TCP and serial transports (slice 4), the driver process with its threads, commands, the generation
change and reload. `docs/decisions.md` D-130 to D-140 record the choices
the contracts left open.

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
- **Live devices go first; the dead get the rest.** Within a priority,
  devices that answer are polled before failing ones and, once one has
  failed, ones not yet answered. The hold ends for the channel 4 time-outs
  of the last failing request after it was sent, and then the oldest
  failing poll goes; until then a failing poll uses a gap only if it is
  longer than its time-out. With the default keys, 250 units at
  38,400 bit/s with 50 dead keep a worst live gap of 2.0 s (1 s group);
  the dead are COMM_FAIL within 77 s and all demoted after 1,674 s (D-138;
  `ctest --preset perf`).
- **Probes use the windows between polls.** A probe goes when no request
  waits, its time-out ended before the next due poll if the window is at
  least 100 ms; one such probe per due time, and its time-out is no
  failure. Else, and after it, the probe waits up to 10 time-outs and then
  goes with its whole time-out (1 s at most), holding one poll, also on a
  line where a request is always due (D-138).
- **Dead values go bad on time.** A poll's points are COMM_FAIL at the
  latest one period (or its retries' time-outs, if longer) after its first
  time-out, even while its retries wait behind live devices (D-140).
- **Units that die together cost one long gap.** Each is asked once in its
  normal turn before it is known to be dead: 50 units with a 1 s time-out
  hold the live ones once for about 51 s (D-138).
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

## Testing

```
cmake --build --preset dev --target scada-driver-unit-test scada-driver-zones-test
ctest --preset unit -R 'scada-driver|sample_processing|tzif'
ctest --preset integration -R scada-driver-zones
```

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
