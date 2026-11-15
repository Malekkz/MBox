# libscada-tagd

The service `scada-tagd` as a library (reference section 5): it creates the
tag database from the tag-set definition and watches the liveness of every
process. The executable in `services/scada-tagd` only calls it.

## What it does

- Reads the tag-set file (the temporary JSON format of phase 1, see
  `libs/scada-tag/README.md`).
- Starts the generation through `scada::tag::GenerationHost`: reuses the
  current one if the tag set is unchanged, otherwise creates the next one
  and switches `current` to it.
- Reports readiness to the service manager once the generation exists.
- In one loop, every `heartbeat_check_ms` (250 ms by default): writes the
  heartbeat of `proc.tagd`, sets and clears the `down` flag of every
  process, removes entries that do not belong in the generation directory
  and logs every change. The same loop feeds the watchdog four times in its
  interval. A hung loop therefore stops the heartbeat and the watchdog
  together.
- On `SIGTERM` or `SIGINT`: marks itself as stopping and exits. The
  generation stays, so that the other services keep running.

It does not touch the shared memory itself; that is in `libscada-tag`. It
does not persist retained values and does not remove an old generation while
it runs; both come with issue P1-09.

## Log records

Component `scada-tagd`. To the journal under a service manager, otherwise to
the standard error stream.

| Message | Level | Fields |
| --- | --- | --- |
| `generation created`, `generation reused` | NOTICE | `GENERATION` |
| A note of the start, for example `gen-3 is not reused: the tag set changed` or `gen-2 was removed` | NOTICE | |
| `process down` | WARNING | `PROCESS`, `REASON`: `region missing`, `not running` or `heartbeat stale` |
| `process up` | NOTICE | `PROCESS` |
| `process restarted` | WARNING if a life came and went between two checks, else NOTICE. Never rate limited | `PROCESS`, `RESTARTS` (lives since the previous sample), `RESTARTS_UNSEEN` (of those, the lives no check saw as down and up), `STARTS_TOTAL` (since the region was created; wraps) |
| `heartbeat late` | WARNING, at most once per process in 10 s; growth in between is summed into the next record. Readers saw the process down for a moment, also when no check fell into it. The figures are reported by the process itself and are a lower bound; a hung process records nothing | `PROCESS`, `LATE_BEATS` (since the previous record of this process), `LATE_BEATS_TOTAL` (since the region was created; wraps), `LONGEST_GAP_MS` (ms), `LAST_LATE_MONO_US` (µs, CLOCK_MONOTONIC, as journald's `__MONOTONIC_TIMESTAMP`), `CHECKS` (checks the record sums) |
| `heartbeat late before this check` | INFO, once, at the first look at a region that holds late heartbeats already | as `heartbeat late` |
| `removed an entry that does not belong in the generation directory` | WARNING | `ENTRY` |
| `cannot remove an entry that does not belong in the generation directory` | ERROR, once for each entry | `ENTRY` |
| `cannot start the tag database` and other errors of the start | CRITICAL | `STATUS`, `DETAIL` |
| `stopped on request` | NOTICE | |

### Message IDs

The liveness records carry a fixed journald `MESSAGE_ID` as their last field
(decision D-47), so that they can be found without matching the text:

| Message | `MESSAGE_ID` |
| --- | --- |
| `process down` | `51ad850aac4848fab6eade0000eaa868` |
| `process up` | `3e293a9e793b4fa08fafb02b5f16ae7f` |
| `process restarted` | `f35dfeb8e8124cc88b219cabf6947b00` |
| `heartbeat late` | `31aad0d613b645d39a83e7978c4531ec` |
| `heartbeat late before this check` | `c4de2438d3574ca38f45771de9254e3a` |

```
journalctl MESSAGE_ID=31aad0d613b645d39a83e7978c4531ec -o verbose
```

- The IDs and fields are a provisional diagnostic input for the alarm service
  of a later phase, which is expected to read `scada_proc_status` or system
  tags instead. They are not an authority: any local process can write a
  journal record with the same fields.
- When the journal does not take a datagram, the record goes to the standard
  error stream as one line; the ID and the fields are then text in the
  message, not journal fields.
- journald limits records per unit (default 10,000 in 30 s; the unit template
  sets no `LogRateLimit*`) and drops beyond it while the sender sees success.
  The 10 s limit of `heartbeat late` keeps `scada-tagd` below it;
  `LATE_BEATS_TOTAL` lets a reader see records that were lost.

A new generation starts with every other process down, so a process that has
not attached yet is not logged; each is logged `process up` once it has
attached, also when that happens before the first check. A reused generation
keeps what the previous `scada-tagd` recorded and logs only changes from it.

## Interfaces

- Contract: `docs/contracts/tag-database.md`, version 0.14, sections 3, 10
  and 11.1; decisions D-27, D-36 and D-47 in `docs/decisions.md`.
- Public header: `include/scada/tagd/tagd.h`, namespace `scada::tagd`:
  `run(args, out, err)` is the whole command.
- Uses `libscada-tag` (`generation_host.h`, `tag_set_json.h`),
  `libscada-service` (readiness, watchdog) and `libscada-common` (log).
- Files: the tag-set file, `/etc/scada/tag-set.json` by default; the tag
  database under `/dev/shm/scada`.

## How to use it

Link the CMake target `scada::tagd`:

```cmake
scada_add_executable(scada-tagd SOURCES src/main.cpp DEPENDS scada::tagd)
```

```cpp
#include "scada/tagd/tagd.h"

return scada::tagd::run(args, std::cout, std::cerr);
```

## How to test it

```
ctest --preset dev -R scada-tagd
```

- `scada-tagd-unit-test`: the command line and the errors found before the
  service starts (`tests/tagd_test.cpp`), the rate limit of `heartbeat late`
  (`tests/late_log_test.cpp`), and every liveness record with its ID and
  fields, sent to a datagram socket that stands in for the journal
  (`tests/report_log_test.cpp`).
- `scada-tagd-integration-test`
  (`tests/integration/tagd_integration_test.cpp`): the running service; see
  `services/scada-tagd/README.md`.
- The generation, reuse, liveness and sweep logic is tested in
  `libs/scada-tag/tests/generation_host_test.cpp`.
