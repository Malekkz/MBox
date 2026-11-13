# scada-hello

A thin executable over `libscada-hello`. It proves that the build produces a
runnable program on x86-64 and ARM64 and that the service skeleton works
(reference 4.2). It has no function in the product.

## What it does

- With no arguments: prints one greeting line and exits with code 0.
- With `--service`: runs as a systemd service of type `notify`. It prints the
  greeting, reports readiness, and feeds the watchdog at half the watchdog
  interval. It stops with exit code 0 on `SIGTERM` or `SIGINT`.
- With `--service`, `SIGUSR1` hangs its main loop, so that the restart by the
  systemd watchdog can be tested.
- Exits with code 2 and a usage text on unknown or surplus arguments, and with
  code 1 on invalid values in `NOTIFY_SOCKET`, `WATCHDOG_USEC` or
  `WATCHDOG_PID`.

`main()` only converts the arguments and calls `scada::hello::run`.

## How to run it

```
build/dev/services/scada-hello/scada-hello [--help | --service]
```

Installed, it runs as `scada-hello.service` under the user `scada-hello`, with
a watchdog interval of 10 s; see `docs/install.md`.

## How to test it

```
ctest --preset dev -R scada-hello-integration-test
```

The integration test (`tests/integration/hello_integration_test.cpp`) starts
the real executable and checks its output and exit code. For the service it
uses a `NotifyListener` (`libscada-service`) in place of systemd, and checks
readiness, watchdog feeding, the stop on `SIGTERM`, and that a hung process
stops feeding and is ended by the watchdog's `SIGABRT`. It starts the service
through the `env` utility, which must be on `PATH`. The restart itself is done
by systemd and is checked on a test machine (`docs/install.md`, step 5).
