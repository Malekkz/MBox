# scada-hello

A thin executable over `libscada-hello`. It proves that the build produces a
runnable program on x86-64 and ARM64 and that the service skeleton works
(reference 4.2). It has no function in the product.

## What it does

- Runs as a systemd service of type `notify`: prints one greeting line,
  reports readiness, and feeds the watchdog at half the watchdog interval.
- Stops with exit code 0 on `SIGTERM` or `SIGINT`.
- Hangs its main loop on `SIGUSR1`, so that the restart by the systemd
  watchdog can be tested.
- Exits with code 2 and a usage text on unknown or surplus arguments, and with
  code 1 on invalid values in `NOTIFY_SOCKET`, `WATCHDOG_USEC` or
  `WATCHDOG_PID`.

`main()` only converts the arguments and calls `scada::hello::run`.

## How to run it

```
build/dev/services/scada-hello/scada-hello [--help]
```

Started by hand it runs without a service manager until it is interrupted.
Installed, it runs as `scada-hello.service` under the user `scada-hello`, with
a watchdog interval of 10 s; see `docs/install.md`.

## How to test it

```
ctest --preset dev -R '^scada-hello-'
```

`scada-hello-service-test` starts the real executable with a `NotifyListener`
(`libscada-service`) in place of systemd. It checks readiness, watchdog
feeding, the stop on `SIGTERM`, and that a hung process stops feeding and is
ended by the watchdog's `SIGABRT`. The restart itself is done by systemd and
is checked on a test machine (`docs/install.md`, step 5). The test starts the
service through the `env` utility, which must be on `PATH`.
