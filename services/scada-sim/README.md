# scada-sim

The simulator writer (issue P1-10): a thin executable over `libscada-sim`.
It stands in for drivers in tests and benchmarks of the tag database, and
gives `scadactl tag watch` something to show. It is a test tool, not part
of the product image: only the install component `test-tools` holds it
(`cmake --install <build> --component test-tools --prefix <directory>`,
which puts it in `usr/bin` under that directory), and it has no service
unit. It is started by hand or by a test, never at boot.

## How to run it

```
build/dev/services/scada-sim/scada-sim --help
build/dev/services/scada-sim/scada-sim tag-set [<layout>] [options] [--output <file>]
build/dev/services/scada-sim/scada-sim run [<layout>] [options]
```

Give both commands the same `<layout>`; the processes, owners and tags of
`run` are found by the names the layout gives.

| Layout option | Default | Meaning |
| --- | --- | --- |
| `--prefix <name>` | `sim` | Process name, or prefix of `<prefix><n>` if there are several processes |
| `--processes <n>` | 1 | Simulator processes |
| `--owners <n>` | 1 | Owners of all processes together, a multiple of the processes; each owner is a thread |
| `--tags <n>` | 1000 | Tags of all owners together, at most 1,000,000 |
| `--pattern <list>` | `check` | Comma-separated: `check`, `ramp`, `sine`, `step`, `walk`; given to the tags of each owner in turn |

| `tag-set` option | Default | Meaning |
| --- | --- | --- |
| `--uid <n>` | the user running it | User id of the simulator processes and readers |
| `--tagd-uid <n>` | `--uid` | User id of `scada-tagd` |
| `--reader <name>` | none | A further process without tags; may be repeated, at most 64 |
| `--heartbeat-check-ms <n>` | 250 | How often `scada-tagd` checks heartbeats |
| `--heartbeat-timeout-ms <n>` | 3000 | Heartbeat timeout of every process |
| `--ring-capacity <n>` | 65,536, or more for an owner with more than 32,768 tags | Change ring entries of every owner |
| `--output <file>` | standard output | Where to write the tag-set file |

| `run` option | Default | Meaning |
| --- | --- | --- |
| `--instance <n>` | 0 | Which simulator process of the layout this is |
| `--rate <n>` | tags / 10 | Updates per second of the whole simulation, shared out by tag count; 0: as fast as possible |
| `--rounds <n>` | none | Stop writing once every tag is written n times |
| `--seconds <n>` | none | Stop writing after n seconds |
| `--hold` | off | After attaching and resolving, print `ready` and wait for SIGUSR1 before the first write |
| `--notify batch\|each` | `batch` | Wake consumers after each 1 ms tick's writes, or after every write |
| `--amplitude <x>` | 100 | Amplitude of the waveforms, above 0 and at most 10^12 |
| `--period <n>` | 100 | Writes of one tag per cycle of a waveform, at least 2 |
| `--seed <n>` | 1 | Seed of `walk` |
| `--base-dir <directory>` | `/dev/shm/scada` | The tag database |
| `--tagd-uid <n>` | the user `scada-tagd` | User id `scada-tagd` runs as |

`run` prints:

- `attached` once attached; with `--hold`, `ready` once every tag is
  resolved, before it waits for SIGUSR1; `writing` when the owner threads
  start;
- `done updates=<n> elapsed_ms=<n> behind_max_ms=<n>` when a run bounded
  by `--rounds` or `--seconds` has ended. `elapsed_ms` is the time of the
  slowest owner; `behind_max_ms` the worst lateness of an update.
- `stopped updates=<n> elapsed_ms=<n>` if SIGTERM, SIGINT or SIGHUP came
  before; `stopped updates=0 elapsed_ms=0` if it came while held.

It stays attached and writes its heartbeat until SIGTERM, SIGINT or SIGHUP.
It stops by itself once `scada-tagd` has been down for 5 s (a shorter
outage, such as a restart of `scada-tagd`, does not stop it) or the
generation retires, while held by `--hold` too, so that a simulator whose
test died does not wait for ever. A held simulator writes its heartbeat; a
SIGUSR1 after the release does nothing. A standard output whose reader is gone does not stop
it: the writes to it fail and are ignored.

`scada-sim run` starts no processes. A layout with `--processes n` needs n
instances, `--instance 0` to `--instance n-1`, started by the caller.

Exit codes: 0 done or stopped on request, 1 error (tag set not valid, file
not written, attach failed, a tag of the wrong type or another process, a
write failed, the generation is retiring, `scada-tagd` down for 5 s), 2
wrong arguments.

To try it without installing, as any user, with 50,000 tags across 10
owners at 50,000 updates per second for 10 s:

```
mkdir -m 2750 /tmp/scada-base
sim=build/dev/services/scada-sim/scada-sim
layout="--prefix load --owners 10 --tags 50000"
$sim tag-set $layout --output /tmp/tag-set.json
build/dev/services/scada-tagd/scada-tagd --tag-set /tmp/tag-set.json --base-dir /tmp/scada-base &
$sim run $layout --rate 50000 --rounds 10 --base-dir /tmp/scada-base --tagd-uid $(id -u)
```

## How to test it

See `libs/scada-sim/README.md`, "How to test it".
