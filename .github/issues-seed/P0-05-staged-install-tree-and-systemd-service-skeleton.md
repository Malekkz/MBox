title: P0-05 Staged install tree and systemd service skeleton
labels: phase-0
---

**Depends on:** P0-01

## Scope

- `cmake --install` produces a staged tree with the directory layout of reference 4.4, under a prefix.
- A systemd unit template for services: `Type=notify`, `WatchdogSec`, own user, and the sandboxing options of reference 4.2.
- `scada-hello` becomes a notify service that reports readiness and feeds the watchdog.
- A short document on installing the staged tree on a test machine by hand. No package format is chosen yet.

## Acceptance

- [ ] `systemd-analyze verify` passes for the generated unit.
- [ ] On a test machine, the service starts, reports ready, and is restarted by systemd when its main loop is deliberately hung.
- [ ] The service runs with a read-only root filesystem.

## References

reference 4.2, 4.4, 14.1, 14.3
