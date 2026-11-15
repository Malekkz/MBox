# scadactl

The local command-line tool (reference section 11.5). In phase 1 it reads
the shared-memory tag database directly, read-only: `status`, `tag list`,
`tag get` and `tag watch`. It has no command that writes. It is the seed of
emergency direct mode (reference 11.6), which phase 10 completes; the API
mode of section 11.5 comes with phase 4.

## What it does

`scadactl` opens an observer context of `libscada-tag` (tag database
contract, section 11.4, decision D-42). An observer needs no entry in the tag
set, writes nothing to shared memory and maps every region read-only. Any
number of `scadactl` may run beside the services.

| Command | Shows |
| --- | --- |
| `status` | Generation and state, whether `scada-tagd` is up, whether reading the clocks is a system call, every process (state, liveness and its reason, heartbeat age, timeout, the `down` flag of `scada-tagd`), every owner (process, tag count, `OWNER_DOWN`). No uid and no pid: they identify accounts and processes, and the output may reach readers who should not see them |
| `tag list [<pattern>]` | Every matching tag in name order: type, value, quality, flags, age, owner |
| `tag get <name>` | One tag, one field per line, with source and update times and change count |
| `tag watch <pattern>` | The current value of every matching tag, then each change as its owner writes it, until SIGINT or SIGTERM (or `--count <n>` changes) |

Output is for a person by default and JSON with `--json`: one document for
`status`, `tag list` and `tag get`, one document per line for `tag watch`.
`scadactl --help` documents every command, option, mark and exit code.

Bad and stale values are marked in both forms:

| | Text | JSON |
| --- | --- | --- |
| BAD (OWNER_DOWN included), or no value could be read | `!` in the first column | `"bad": true` |
| UNCERTAIN, or STALE | `~` in the first column | `"stale": true` for STALE; `"quality": "UNCERTAIN"` |
| No value (never written, owner missing) | value `-`, age `-` | `"value": null`, `"age_ns": null` |

In `status`, a process or owner that is down starts with `!`.

When `scada-tagd` is down its liveness check has stopped, so a value may be
stale without a mark: `tag list` and `tag get` then start with `! scada-tagd
down: values may be stale` and carry `"tagd_down": true` in JSON, and `tag
watch` writes such a line (or `{"event":"tagd","down":...}`) when it goes
down or comes back.

Nothing read from a generation that was retired meanwhile is shown.
`scadactl` calls `scada_tag_status` after it reads and before it shows
anything (contract 11.4): `tag list` and `tag get` read again from the new
generation, and `tag watch` drops what it held, observes again, prints a
`generation` line and starts over.

Text from shared memory is input: names and string values are escaped so
that a terminal shows them and does nothing else (control characters,
C1 controls and direction overrides become `\xNN` or `\uNNNN`), and so
that JSON stays valid (bytes that are not UTF-8 become U+FFFD).

`tag watch` subscribes to the change ring of every owner of a matching tag
before it reads the tags, so that nothing between the two is lost. After a
gap it prints a `gap` line with the reason, reads that owner's tags again
and drops any later change older than what it read (contract 8.3). It
waits at most a second at a time and calls `scada_tag_status` before it
shows what it took from the rings; on a generation change it closes its
subscriptions, detaches, observes again and starts over.

## Who may run it

A member of the group `scada`, or root. Anyone else gets `no permission to
read the tag database`.

Membership of `scada` means unaudited read access to every tag: these reads
are recorded nowhere. It also allows creating files in the generation
directory and locking the `proc` region of a process, which can keep that
process from attaching. `scadactl` does neither; the power belongs to the
group, not to this tool. See `docs/install.md`, section 8.

## How to run it

```
scadactl status
scadactl tag list 'plant.*'
scadactl tag get plant.pump1.speed
scadactl --json tag watch 'plant.pump1.*'
scadactl --base-dir /tmp/db --tagd-uid 1000 status   # a database of a test
```

Exit codes: 0 success, 1 error (no database, no such tag, no access), 2
wrong arguments.

## Structure

`libscadactl` holds all logic; `src/main.cpp` only hands it the arguments.

| File | Content |
| --- | --- |
| `arguments.h` | The command line and the help text |
| `pattern.h` | Tag-name globs: `*` any characters, dots included; `?` one |
| `format.h` | Values, qualities, flags, ages, times; terminal-safe and JSON text |
| `view.h` | What is shown, as plain rows, and both output forms |
| `database.h` | The observer context: find, read, status, observe again |
| `watch.h` | `tag watch` |
| `scadactl.h` | `run`: the whole command |

## How to test it

```
ctest --preset dev -R scadactl-unit-test
ctest --preset dev -R scadactl-integration-test
```

- `scadactl-unit-test`: the command line, every refusal of it and the help
  text (`Arguments.*`); patterns (`TagPattern.*`); values of every type,
  marks, flags, ages, times, terminal-safe and JSON text, with control
  characters, C1 controls, direction overrides, surrogates and overlong
  forms (`Format.*`); both forms of every table, detail, watch, gap and
  generation line, and the error messages (`TagTable.*`, `TagJson.*`,
  `Status.*`, `Watch.*` and others in `view_test.cpp`).
- `scadactl-integration-test` (in `tests/integration/`) starts `scadactl`
  as a program against real `scada-tagd`, peer and writer processes:
  help, no database, `status` with a process up and one missing, `tag
  list` and `tag get` of values that are BAD, `tag watch` of a live writer
  in both forms with BAD, UNCERTAIN and STALE values and their age, a stop
  by SIGINT, a generation change, and a pattern that matches nothing.
- The acceptance of issue #19 names `scada-sim` as the writer for `tag
  watch`. Until `scada-sim` (#18) is on main, the test writer of
  `libscada-tag` plays it; its n-th write stores quality n % 3 and flags
  n & 0x0FDF, so GOOD, UNCERTAIN, BAD and STALE all come.
