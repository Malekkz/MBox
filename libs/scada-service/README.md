# libscada-service

What a process needs to run as a systemd service of type `notify`
(reference 4.2): reporting readiness and feeding the watchdog.

## What it does

- Reads and validates `NOTIFY_SOCKET`, `WATCHDOG_USEC` and `WATCHDOG_PID`.
- Sends `READY=1`, `WATCHDOG=1`, `STOPPING=1` and `STATUS=<text>` to the
  service manager. Sending never blocks.
- Provides `NotifyListener`, the service manager's side of the protocol, so
  tests can start a service and observe it without systemd.
- It does not link `libsystemd` (`docs/decisions.md`, D-15), and it does not
  decide when to feed the watchdog; the service's main loop does.

## Interfaces

- Public headers in `include/scada/service/`, namespace `scada::service`:
  `notify.h`, `notify_listener.h`, `fd.h`.
- The systemd notification protocol, as described in `sd_notify(3)`.
- No contract of this project.

## How to use it

Link the CMake target `scada::service`:

```cpp
#include "scada/service/notify.h"

const auto settings = scada::service::parse_notify_environment(
    scada::service::read_notify_environment(), ::getpid());
if (!settings) { /* invalid environment: refuse to start */ }

scada::service::Notifier notifier(*settings);
// ... start up ...
if (notifier.ready() == scada::service::NotifyStatus::FAILED) { /* exit */ }
// In the main loop, at half of settings->watchdog_interval:
if (notifier.watchdog() == scada::service::NotifyStatus::FAILED) { /* log */ }
```

Install the unit and the service user with `scada_install_service` in the
service's `CMakeLists.txt` (`cmake/ScadaInstall.cmake`).

## How to test it

```
ctest --preset dev -R 'scada-service-unit-test|notify_environment_fuzz'
```

The unit test uses `NotifyListener` on an abstract-namespace socket; it needs
no systemd and creates no files. `fuzz/notify_environment_fuzz.cpp` is the
fuzz target of the environment parser; its seed corpus is replayed as a unit
test, and it is fuzzed in the `fuzz` build.
