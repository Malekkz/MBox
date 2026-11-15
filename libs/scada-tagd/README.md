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
  is BAD or NOT_INIT removes the tag from the file; one that cannot be read
  consistently (mid-write) leaves the value on file. No FINAL file is
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
| `heartbeat late` | WARNING, at most once per process and check. Readers saw the process down for a moment, also when no check fell into it. The figures are reported by the process itself and are a lower bound; a hung process records nothing | `PROCESS`, `LATE_BEATS` (since the previous check), `LATE_BEATS_TOTAL`, `LONGEST_GAP_MS`, `LAST_LATE_MONO_NS` (CLOCK_MONOTONIC) |
| `heartbeat late before this check` | INFO, once, at the first look at a region that holds late heartbeats already | as `heartbeat late` |
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

A new generation starts with every other process down, so a process that has
not attached yet is not logged; each is logged `process up` once it has
attached, also when that happens before the first check. A reused generation
keeps what the previous `scada-tagd` recorded and logs only changes from it.

## Interfaces

- Contract: `docs/contracts/tag-database.md`, version 0.5, sections 3, 10
  and 11.1; decision D-27 in `docs/decisions.md`.
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

- `scada-tagd-unit-test` (`tests/tagd_test.cpp`): the command line and the
  errors found before the service starts.
- `scada-tagd-integration-test`
  (`tests/integration/tagd_integration_test.cpp`): the running service; see
  `services/scada-tagd/README.md`.
- The generation, reuse, liveness and sweep logic is tested in
  `libs/scada-tag/tests/generation_host_test.cpp`.
