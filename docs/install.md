# Installing the staged tree by hand

This installs a build on a test machine without a package. No package format
is chosen yet (reference 14.1 and 19.1); packaging will be a thin layer over
the same tree.

The test machine needs 64-bit Linux with systemd 250 or newer, the same
architecture as the build, and a C++ runtime library at least as new as the
one of the build machine. All commands on the test machine run as root.

## 1. Build the staged tree

On the build machine:

```
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix stage
```

For ARM64 use the preset `release-arm64` and `build/release-arm64`.

`stage/` is now an image of the paths the software owns on the target
(reference 4.4):

| Path under `stage/` | Content |
| --- | --- |
| `usr/bin/` | Service executables |
| `usr/lib/scada/` | Driver plugins (empty for now) |
| `usr/share/scada/web/` | Web app static files (empty for now) |
| `usr/share/doc/scada/` | Licence report and software bill of materials |
| `usr/lib/systemd/system/` | One systemd unit per service |
| `usr/lib/sysusers.d/` | The `scada` group and one system user per service |
| `usr/lib/tmpfiles.d/` | Directories created at every boot: `/dev/shm/scada` |
| `etc/scada/` | Active configuration (empty for now) |
| `var/lib/scada/{config-history,hist,journal,retain}/` | Runtime data (empty) |

`/run/scada` and `/dev/shm/scada` are on memory-backed filesystems and are
not in the tree. `/dev/shm/scada` is created at every boot from
`usr/lib/tmpfiles.d/scada-tagd.conf`; `/run/scada` is created at runtime by
the services that use it.

## 2. Copy it to the test machine

```
tar -C stage -cf scada-stage.tar .
```

Transfer `scada-stage.tar`, then on the test machine:

```
tar -C / -xf scada-stage.tar --no-same-owner --no-overwrite-dir
systemd-sysusers
systemd-tmpfiles --create scada-tagd.conf
systemctl daemon-reload
```

`--no-same-owner` makes every file owned by root. `--no-overwrite-dir` keeps
the permissions of directories that already exist, such as `/usr`.
`systemd-sysusers` creates the group and the service users.
`systemd-tmpfiles` creates `/dev/shm/scada` now; after a reboot systemd
creates it by itself.

## 3. Check the units

```
systemd-analyze verify scada-hello.service scada-tagd.service
```

It must print nothing and exit with code 0.

## 4. Start the service and see it report ready

```
systemctl start scada-hello.service
systemctl status scada-hello.service
```

The unit runs `scada-hello --service`. `systemctl start` returns only after
the service has reported readiness, because the unit is of type `notify`. The status shows `active (running)` and
the user `scada-hello`. `journalctl -u scada-hello.service` shows the line
`Hello from libscada-hello`.

## 5. Hang the main loop and see systemd restart it

`scada-hello` stops feeding the watchdog when it receives `SIGUSR1`:

```
systemctl show -p MainPID -p NRestarts scada-hello.service
systemctl kill --signal=SIGUSR1 scada-hello.service
```

The watchdog interval is 10 s and the restart delay 1 s. Within about 12 s:

```
journalctl -u scada-hello.service -n 20
systemctl show -p MainPID -p NRestarts scada-hello.service
```

The journal shows `Watchdog timeout`, the process killed with `SIGABRT`, and a
new start. `MainPID` has changed and `NRestarts` is one higher.

## 6. Run with a read-only root filesystem

The service writes nothing, and `ProtectSystem=strict` already makes the whole
filesystem read-only for it. To show that it also runs when the root
filesystem itself is read-only:

```
systemctl stop scada-hello.service
mount -o remount,ro /
systemctl start scada-hello.service
systemctl status scada-hello.service
mount -o remount,rw /
```

If the remount is refused because another program holds a file open for
writing, stop that program or boot the machine with `ro` on the kernel command
line.

## 7. Start the tag database daemon

### Disable system sleep first

The unit must never suspend, hibernate or hybrid-sleep (contract 3.4,
decision D-45): the monotonic clock stops meanwhile, so a write request
could be executed after a resume, later than its requester allowed. These
lines are for the test machine, run as root; they use only systemd, which
this guide requires, and a package will ship the same settings later. Mask
the sleep targets, and tell systemd's sleep configuration to refuse every
form of sleep:

```
systemctl mask sleep.target suspend.target hibernate.target \
    hybrid-sleep.target suspend-then-hibernate.target
mkdir -p /etc/systemd/sleep.conf.d
cat > /etc/systemd/sleep.conf.d/scada.conf <<END
[Sleep]
AllowSuspend=no
AllowHibernation=no
AllowHybridSleep=no
AllowSuspendThenHibernate=no
END
```

`scada-tagd` cannot see this setting. At start it logs, once at INFO, the
sleep states the kernel offers (`/sys/power/state`); that line is expected,
since most kernels offer them even with the targets masked. It logs a
WARNING `the system was suspended` (or `... since boot` at start) with
`SUSPENDED_MS` when it measures that the system slept. A virtual machine that is paused is not seen: its clocks stop
too. Do not pause a virtual machine that runs the unit.

### Start it

`scada-tagd` creates the shared-memory tag database from a tag-set file. In
phase 1 that file is written by hand and names users by number
(`libs/scada-tag/README.md`). The smallest one holds only `scada-tagd`
itself:

```
cat > /etc/scada/tag-set.json <<END
{"format": 1,
 "processes": [{"name": "tagd", "uid": $(id -u scada-tagd)}],
 "owners": [], "queues": [], "tags": []}
END
systemctl start scada-tagd.service
systemctl status scada-tagd.service
ls -ln /dev/shm/scada /dev/shm/scada/current/
```

The status shows `active (running)`, the user `scada-tagd` and the status
line `Generation 1`. The directory listing shows:

| Entry | Mode | Owner and group |
| --- | --- | --- |
| `/dev/shm/scada` | `drwxr-s---` (2750) | `scada-tagd`, `scada` |
| `current -> gen-1` | a symbolic link | `scada-tagd`, `scada` |
| `gen-1` | `drwxrws--T` (3770) | `scada-tagd`, `scada` |
| `gen-1/header`, `gen-1/proc.tagd` | `-rw-r-----` (0640) | `scada-tagd`, `scada` |

Restart it and see that the generation is reused:

```
systemctl restart scada-tagd.service
journalctl -u scada-tagd.service -n 5
readlink /dev/shm/scada/current
```

The journal shows `generation reused` with `GENERATION=1`, and `current`
still points to `gen-1`. After a change to the tag-set file, a restart shows
`generation created` with the next number.

The service needs no capability: `systemctl show -p CapabilityBoundingSet
scada-tagd.service` prints an empty value.

## 8. Look at the tag database with scadactl

`scadactl` reads the tag database directly and only reads
(`tools/scadactl/README.md`). It runs as root, or as a user in the group
`scada`:

```
scadactl status
scadactl tag list
scadactl tag watch 'plant.*'      # Ctrl-C stops it
```

`status` shows generation 1, `scada-tagd up`, and the process `tagd`
RUNNING. A user outside the group gets `no permission to read the tag
database`.

Membership of the group `scada` means unaudited read access to every tag:
`scadactl` records nothing, and neither does the library. It also allows
creating files in the generation directory and locking the `proc` region
of a process, which can keep that process from attaching. That is a power
the group already has for the services; `scadactl` uses neither. Add only
accounts to `scada` that are trusted that far (decision D-42).

## 9. Remove it again

```
systemctl stop scada-hello.service scada-tagd.service
rm -rf /dev/shm/scada
tar -tf scada-stage.tar | grep -v '/$' | sed 's|^\./|/|' | xargs rm -f
rm -rf /etc/scada /var/lib/scada /usr/lib/scada /usr/share/scada
systemctl daemon-reload
userdel scada-hello
userdel scada-tagd
groupdel scada
```

The tar listing is used so that only files that came from the tree are
removed. `rm -rf /var/lib/scada` deletes all runtime data.
