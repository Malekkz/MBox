# scada-tagd

The tag database daemon (reference section 5): a thin executable over
`libscada-tagd`. Every other service depends on it (reference 3.3).

## What it does

- Creates the shared-memory tag database from the tag-set file: the
  generation directory, the `header` region and the link `current`.
- Reuses the current generation when the tag set is unchanged, so that a
  restart of `scada-tagd` does not disturb the other services.
- Checks every `heartbeat_check_ms` (250 ms by default) which processes are
  down, sets and clears their `down` flag, and logs every change. The first
  check of a new generation also logs each process that has not attached.
- Removes from the generation directory what the header does not list or
  what belongs to the wrong user.
- Exit codes: 0 stopped on request (`SIGTERM`, `SIGINT`), 1 error, 2 wrong
  arguments.

`main()` only converts the arguments and calls `scada::tagd::run`. The
details are in `libs/scada-tagd/README.md`.

## How to run it

```
build/dev/services/scada-tagd/scada-tagd [--tag-set <file>] [--base-dir <directory>]
    [--retain-dir <directory>] [--retain-interval-ms <ms>]
build/dev/services/scada-tagd/scada-tagd --help
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--tag-set <file>` | `/etc/scada/tag-set.json` | The tag-set file. It must hold a process named `tagd` whose `uid` is the user `scada-tagd` runs as. |
| `--base-dir <directory>` | `/dev/shm/scada` | The directory of the tag database. It must exist, be owned by the user `scada-tagd` runs as, and have mode `2750`. |
| `--retain-dir <directory>` | `/var/lib/scada/retain` | The directory of the retained values. It must exist, be owned by the user `scada-tagd` runs as, and have mode `2750`; `scada-tagd` does not create it. |
| `--retain-interval-ms <ms>` | `5000` | The shortest time between two writes of the retain file of one owner, 100 to 3,600,000 ms. A change of a `retain_now` tag is written at the next liveness check. |

Installed, it runs as `scada-tagd.service`: a notify service under the user
`scada-tagd`, member of the group `scada`, with a watchdog interval of 10 s
and no capability. `/dev/shm/scada` is created at boot from
`usr/lib/tmpfiles.d/scada-tagd.conf`, which also gives
`/var/lib/scada/retain` its owner and mode; the drop-in
`scada-tagd.service.d/retain.conf` lets the service write that directory.
See `docs/install.md`, step 7.

To try it without installing, as any user:

```
mkdir -m 2750 /tmp/scada-base
cat > /tmp/tag-set.json <<END
{"format": 1,
 "processes": [{"name": "tagd", "uid": $(id -u)}],
 "owners": [], "queues": [], "tags": []}
END
build/dev/services/scada-tagd/scada-tagd --tag-set /tmp/tag-set.json --base-dir /tmp/scada-base --retain-dir /tmp/scada-retain
```

## How to test it

```
ctest --preset dev -R scada-tagd-integration-test
```

The integration test (`tests/integration/tagd_integration_test.cpp`) starts
the real executable on a base directory in the temporary directory of the
machine. A `NotifyListener` (`libscada-service`) stands in for systemd, and
the helper `scada-tag-test-peer` (`libs/scada-tag/tests/peer`) stands in for
another service: it attaches through the C interface of `libscada-tag` and
writes its heartbeat. The
`env` utility must be on `PATH`. It checks:

- readiness after the generation exists, watchdog feeding, the stop on
  `SIGTERM`, and the modes of what was created;
- conformance test 5, the part of `scada-tagd`: a process killed with
  `SIGKILL` is logged as down (`heartbeat stale`) and, restarted, as up;
- conformance test 6, the part of `scada-tagd`: a process that stops cleanly
  is logged as down (`not running`);
- a heartbeat that comes later than the timeout is logged as
  `heartbeat late` (issue #42, D-36);
- conformance test 20: after a restart with the same tag set the generation
  is reused, no file is replaced, and the helper is never logged as down;
- attach with real processes: a process creates its regions with mode
  `0640` and the sweep leaves them alone; conformance test 7: an owner
  killed in the middle of a write repairs its region when it is restarted;
  conformance test 10: a second process of the same name is refused; without
  a generation a process cannot attach;
- a changed tag set gives the next generation; a second instance and a base
  directory with the wrong mode are refused.
