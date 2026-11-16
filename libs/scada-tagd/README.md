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
- From the same loop, retained values (contract, section 12; decision
  D-45): the tags marked `retain` are read with the sequence-lock read, and
  the file `<owner>.bin` in the retain directory is written when a value
  changed, at most every `--retain-interval-ms`, at the next check for a
  `retain_now` tag. The files are written by a thread of their own, so that
  a slow `fsync` does not hold up the heartbeat; the removal of old files
  and its directory `fsync` run there too. A record read consistently that
  is BAD, written by the owner (no NOT_INIT), removes the tag from the
  file; one that is NOT_INIT (not written yet, for example after a failed
  restore) leaves the value on file, as does one that cannot be read
  consistently (mid-write). No FINAL file is
  written over newer values: not for an owner that has a ready `tags`
  region in a newer generation, nor one whose file is already FINAL for a
  newer generation.
- Retired generations (contract, section 11.3): every generation in the base
  directory other than the current one is watched, also after a restart.
  Once the process of an owner has left one (its state is not RUNNING, or it
  no longer holds the lock of its `proc` region), that owner's values there
  are written to its file marked FINAL, which its attach to the new
  generation waits for. Once every owner is done and no `proc` region is
  locked, the generation is removed. After 30 s it is removed anyway; the
  values of a process still attached are written, but not marked FINAL. A
  generation that cannot be removed is tried for at most 60 s after it
  retired, then logged as abandoned and left to the next boot.
- Once no generation is retired and every owner has its file, files of
  owners the tag set no longer has are removed.
- On `SIGTERM` or `SIGINT`: writes the retained values that changed,
  waiting for the writer at most 5 s (logged if values stay unsaved), marks
  itself as stopping and exits. The generation stays, so that the other
  services keep running.

It does not touch the shared memory itself; that is in `libscada-tag`.

## Log records

Component `scada-tagd`. To the journal under a service manager, otherwise to
the standard error stream.

| Message | Level | Fields |
| --- | --- | --- |
| `generation created`, `generation reused` | NOTICE | `GENERATION` |
| A note of the start, for example `gen-3 is not reused: the tag set changed` or `gen-2 was removed` | NOTICE | |
| `process down` | WARNING | `PROCESS`, `REASON`: `region missing`, `not running` or `heartbeat stale` |
| `process up` | NOTICE | `PROCESS` |
| `process not attached` | INFO, once for each process, only at the first check of a new generation: the process has no usable region yet | `PROCESS` |
| `process restarted` | WARNING if a life came and went between two checks, else NOTICE. Never rate limited | `PROCESS`, `RESTARTS` (lives since the previous sample), `RESTARTS_UNSEEN` (of those, the lives no check saw as down and up), `STARTS_TOTAL` (since the region was created; wraps) |
| `heartbeat late` | WARNING, at most once per process in 10 s; growth in between is summed into the next record. Readers saw the process down for a moment, also when no check fell into it. The figures are reported by the process itself and are a lower bound; a hung process records nothing | `PROCESS`, `LATE_BEATS` (since the previous record of this process), `LATE_BEATS_TOTAL` (since the region was created; wraps), `LONGEST_GAP_MS` (ms), `LAST_LATE_MONO_US` (µs, CLOCK_MONOTONIC, as journald's `__MONOTONIC_TIMESTAMP`), `CHECKS` (checks the record sums) |
| `heartbeat late before this check` | INFO, once, at the first look at a region that holds late heartbeats already | as `heartbeat late` |
| `queue broken` | WARNING, once when a queue's cursor and `write_seq` become a pair out of range (contract 9.2). The queue refuses posts and delivers nothing until the next generation | `QUEUE`, `CURSOR`, `WRITE_SEQ`, `CAPACITY`, `REASON`: `replaced` (the consumer found the queue file replaced) or `out of range` |
| `queue no longer broken` | NOTICE, once | `QUEUE`, `CURSOR`, `WRITE_SEQ`, `CAPACITY` |
| `queue file missing` | WARNING, once, when an events or command queue has had no usable file at two checks in a row while its producer was up | `QUEUE` |
| `queue file found again` | NOTICE, once | `QUEUE` |
| `queue file created anew; audit records may be lost` (events queue), `queue file created anew; records may be lost` (other queues) | WARNING, once, when the file under a queue's name is another one than at the previous check (contract 9.3, D-49). The records of the old file from the consumer's cursor to its last `write_seq` are lost unless the consumer still holds the old file | `QUEUE`, `BASE_SEQ` (or `none`), `OLD_WRITE_SEQ`, `CURSOR`, `LOST_AT_MOST`, `LOST_IDS` (`[from, to)`), `REASON` |
| `queue write_seq fell; ids may repeat` | ERROR, once for each fall: a queue's `write_seq` is below the value of the previous check in the same file (damage or a second producer, contract 3.3; D-51) | `QUEUE`, `OLD_WRITE_SEQ`, `WRITE_SEQ` |
| `the kernel offers sleep states; the unit must not suspend` | INFO, once at start, when `/sys/power/state` is not empty (contract 3.4). Expected on most kernels, also with sleep disabled | `STATES` |
| `the system was suspended since boot`, `the system was suspended` | WARNING, at start and when it happens: `CLOCK_BOOTTIME` ran ahead of `CLOCK_MONOTONIC` | `SUSPENDED_MS`, a lower bound, rounded up to whole ms |
| `removed an entry that does not belong in the generation directory` | WARNING | `ENTRY` |
| `cannot remove an entry that does not belong in the generation directory` | ERROR, once for each entry | `ENTRY` |
| `cannot start the tag database` and other errors of the start | CRITICAL | `STATUS`, `DETAIL` |
| `retained values written` | DEBUG | `OWNER`, `VALUES` |
| `retained values written after the owner left a retired generation` | INFO | `OWNER`, `VALUES` |
| `retained values` | WARNING, once until the problem is gone: a file that failed its checks and was ignored, or a file that could not be written | `SUBJECT` (file or owner), `DETAIL` |
| `removed the retain file of an owner that is gone` | NOTICE | `FILE` |
| `retired generation removed` | NOTICE | `GENERATION` |
| `retired generation removed after the timeout, with processes still attached` | WARNING | `GENERATION` |
| `process still attached to a retired generation` | WARNING, before such a removal | `GENERATION`, `PROCESS` (`<name> (pid <pid>)`) |
| `cannot remove a retired generation` | ERROR, once | `GENERATION` |
| `stopped on request` | NOTICE | |

### Message IDs

The liveness records carry a fixed journald `MESSAGE_ID` as their last field
(decision D-47), so that they can be found without matching the text:

| Message | `MESSAGE_ID` |
| --- | --- |
| `process down` | `51ad850aac4848fab6eade0000eaa868` |
| `process up` | `3e293a9e793b4fa08fafb02b5f16ae7f` |
| `process restarted` | `f35dfeb8e8124cc88b219cabf6947b00` |
| `process not attached` | `0f57ce3d6dbc4891abd0e55e240f04f9` |
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

A new generation starts with every other process down. The first check logs
`process not attached` for each process that has no region yet. Each process
is logged `process up` once it has attached and is up, also when that happens
before the first check. A reused generation keeps what the previous
`scada-tagd` recorded and logs only changes from it.

## Interfaces

- Contract: `docs/contracts/tag-database.md`, version 0.18, sections 3, 5,
  9.2, 10 and 11.1; decisions D-27, D-36, D-38, D-46, D-47, D-49, D-51 and D-53 in
  `docs/decisions.md`.
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
