# scada-hello

A thin executable over `libscada-hello`. It proves that the build produces a
runnable program on x86-64 and ARM64. It has no function in the product.

## What it does

- Prints one greeting line and exits with code 0.
- Exits with code 2 and a usage text on unknown or surplus arguments.

`main()` only converts the arguments and calls `scada::hello::run`.

## How to run it

```
build/dev/services/scada-hello/scada-hello [--help]
```

## How to test it

```
ctest --preset dev -R '^scada-hello-(runs|rejects)'
```

These tests start the real executable and check its output and exit code.
