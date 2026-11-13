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
| `etc/scada/` | Active configuration (empty for now) |
| `var/lib/scada/{config-history,hist,journal,retain}/` | Runtime data (empty) |

`/run/scada` and `/dev/shm/scada` are on memory-backed filesystems. They are
not in the tree; the services that own them create them at runtime.

## 2. Copy it to the test machine

```
tar -C stage -cf scada-stage.tar .
```

Transfer `scada-stage.tar`, then on the test machine:

```
tar -C / -xf scada-stage.tar --no-same-owner --no-overwrite-dir
systemd-sysusers
systemctl daemon-reload
```

`--no-same-owner` makes every file owned by root. `--no-overwrite-dir` keeps
the permissions of directories that already exist, such as `/usr`.
`systemd-sysusers` creates the group and the service users.

## 3. Check the unit

```
systemd-analyze verify scada-hello.service
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

## 7. Remove it again

```
systemctl stop scada-hello.service
tar -tf scada-stage.tar | grep -v '/$' | sed 's|^\./|/|' | xargs rm -f
rm -rf /etc/scada /var/lib/scada /usr/lib/scada /usr/share/scada
systemctl daemon-reload
userdel scada-hello
groupdel scada
```

The tar listing is used so that only files that came from the tree are
removed. `rm -rf /var/lib/scada` deletes all runtime data.
