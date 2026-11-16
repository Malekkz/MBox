# libscada-sim

The simulator writer of the tag database as a library (issue P1-10;
reference sections 15.3 and 18.2). The executable in `services/scada-sim`
only calls it. Decision D-41 in `docs/decisions.md` records the choices.

## What it does

- Describes a simulation (`Layout`, `scada/sim/layout.h`): how many
  simulator processes, owners and tags, their names, and the pattern of each
  tag. The tag-set generator and every simulator process compute the same
  names from the same layout.
- Makes the tag set of a simulation (`make_tag_set`, `scada/sim/tag_set.h`):
  the process `tagd`, the simulator processes, their owners and tags, and
  reader processes without tags; and writes it in the temporary tag-set
  file format that `scada-tagd` reads (`tag_set_json`). "50,000 tags across
  10 owners" is `--owners 10 --tags 50000`.
- Defines the patterns (`scada/sim/pattern.h`). Every value is a function of
  the tag and its write count, not of the clock, so a test can compute what
  a reader must see.
- Runs one simulator process (`run`, `scada/sim/sim.h`): attaches, resolves
  its tags, checks their types, and writes the tags of each owner from a
  thread of its own at a set rate.

## Names

For prefix `p` (default `sim`):

| What | Name |
| --- | --- |
| Process | `p` if there is one process, else `p<n>`, n from 0 |
| Owner | The process name if the process has one owner, else `<process>.w<k>`, k from 0 within the process |
| Tag | `<owner>.t<i>`, i from 0 within the owner |

Owners are numbered across all processes; owner g belongs to process
g / (owners / processes). The tags are shared out evenly; the first
`tags % owners` owners get one more. Tag i of every owner follows pattern
`i % <number of patterns>` of the list given.

## Patterns

Write number n of a tag, n from 1; tag index i within its owner.

| Pattern | Type | Value | Quality, flags, source time |
| --- | --- | --- | --- |
| `check` | UINT64 | n | quality n % 3, flags n & 0x0FDF (never OWNER_DOWN), source time 1,700,000,000,000,000,000 + n |
| `ramp` | FLOAT64 | A * ((n + i) mod P) / P | GOOD, no flag, no source time (the library sets LOCAL_TIME) |
| `sine` | FLOAT64 | A * sin(2 pi ((n + i) mod P) / P) | as `ramp` |
| `step` | FLOAT64 | 0 while 2 * ((n + i) mod P) < P, else A | as `ramp` |
| `walk` | FLOAT64 | previous + a step of at most 2A / P, from a hash of seed, i and n; held within -A to A; starts at 0 | as `ramp` |

A is `--amplitude` (default 100), P is `--period` (default 100 writes, at
least 2). `check` is the self-checking pattern: its fields are derived
from one another, so a reader that sees fields of two writes can tell.
`is_check_value` judges a value read from a tag (a never-written record is
whole), `is_check_change` an entry of a change ring. Since every write
changes the value, a whole `check` value also has change count n (mod
2^32).

## Rate and pacing

`--rate` is the rate of the whole simulation, all processes together. Each
owner gets the share of its tags, so every tag is written equally often.
An owner thread writes its tags in turn. Every 1 ms it writes what is due
by then, at most 1,024 updates at a time, and calls `scada_tag_notify` after
each batch (`--notify batch`), or after each write (`--notify each`). If it
fell behind it writes on without waiting, but looks at the stop request
after every batch, so that a run asked for more than the machine can write
still stops at once. `behind_max_ms` in the report is the worst lateness of
an update against its due time. `--rate 0` writes as fast as it can, with a notify after each
1,024 writes. Without `--rate` the rate is a tenth of the tags per second,
the change rate of the load model of reference 15.3.

A run ends after `--rounds n` (every tag written n times) or `--seconds n`
(with a rate: rate × n updates, shared out among all owners by their tags
without loss to rounding, each owner writing its share evenly over the
time; at `--rate 0`: whatever was written by then). Without either it
writes until stopped. The process stays attached after the end and writes
its heartbeat until SIGTERM, SIGINT or SIGHUP, so the last values read as
good. It stops with an error once `scada-tagd` has been down for 5 s, so
that a simulator left behind by a test does not write on until it is
killed; a shorter outage, such as a restart of `scada-tagd`, which every
process must tolerate (reference 3.3), does not stop it. With `--hold` the
process waits after resolving, with its heartbeat and the same stop rules,
until SIGUSR1, so that a test can start its readers before the first write
(decision D-50). See `services/scada-sim/README.md` for the
output.

A simulator that attaches again to regions it wrote before goes on with
each `check` tag from the change count of its record, so that change count
and write count stay equal across restarts; `pattern.h` states the one case
after a kill in the middle of a write where they do not.

While `run` resolves and checks its tags, before the owner threads start,
it writes its heartbeat every 256 tags, so that a large layout does not read
as down on a slow machine.

The writes are not real-time: the owner threads run under the normal
scheduler and sleep between ticks.

## Interfaces

| Header | Content |
| --- | --- |
| `scada/sim/pattern.h` | `Pattern`, `Waveform`, `check_value`, `is_check_value`, `is_check_change`, `wave_value` |
| `scada/sim/layout.h` | `Layout`, `check_layout` and the names |
| `scada/sim/tag_set.h` | `TagSetOptions`, `make_tag_set`, `tag_set_json` |
| `scada/sim/sim.h` | `Options`, `parse_options`, `to_tag_set_options`, `run` (the command) |

`src/writer.h` (the owner threads) is private.

## How to test it

```
ctest --preset dev -R 'scada-sim|sim_options'
ctest --preset perf -R scada-sim-rate-perf-test
```

- `scada-sim-unit-test`: names, sharing out and refused layouts; the
  patterns, including that `is_check_value` refuses every field taken from
  another write; the tag set, its limits, and that its file reads back with
  the same `config_hash`; every option, its limits and combinations.
- `sim_options_fuzz` (in `fuzz/`) feeds the command-line parser with
  arbitrary arguments; a `tag-set` command line of up to 4,096 tags that
  parses is made into a tag set and its file, which must read back with the
  same `config_hash`. Its seed corpus runs as `sim_options_fuzz-corpus`.
- `scada-sim-integration-test` (in `tests/integration/`): the file of
  `scada-sim tag-set` is the one `scada-tagd` runs on; after a run every tag
  of every pattern holds exactly the value, quality, flags and change count
  its definition gives; two processes share one simulation; a run of one
  second writes the rate once, and 1 update across 10 owners is not lost to
  rounding; a restarted simulator goes on counting; an unbounded run stops
  on SIGTERM, and SIGHUP stops it the same way; a run asked for
  100,000,000 updates per second stops within 1 s of SIGTERM and keeps its
  heartbeat; a simulator whose output reader is gone survives (no SIGPIPE)
  and exits with code 1 once `scada-tagd` has been down for 5 s; 40,000
  tags with a heartbeat timeout of 100 ms never read as down while the
  simulator starts; a tag set that does not fit the run is refused.
- `scada-sim-rate-perf-test` (in `tests/perf/`): 50,000 updates per second
  across 10 owners on 50,000 tags, as one process with 10 owner threads and
  as 10 processes, for 10 s each. On x86-64 it fails if an update is
  missing, a run takes more than 10.1 s, or an update is more than 250 ms
  late, and if one process writing as fast as it can stays below 50,000
  updates per second; on ARM64 it reports the figures only.
- The conformance tests 2, 3, 13 and 24 of the tag database and the
  wake-up perf test use `scada-sim` as their writer
  (`tests/integration/tag_processes_fixture.h`). `scada-sim` always calls
  notify, so the part of test 24 for a process that never notifies (the
  100 ms cap) is not driven by it.

## Installing it

`scada-sim` is not part of the product image. It is in the CMake install
component `test-tools`, which a plain `cmake --install` leaves out:

```
cmake --install build/dev --component test-tools --prefix <directory>
```
