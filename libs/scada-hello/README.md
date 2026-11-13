# libscada-hello

A trivial library that proves the build and the library-plus-thin-executable
pattern (CLAUDE.md, "How work is organised"). It has no function in the
product.

## What it does

- Returns a fixed greeting line.
- Implements the whole `scada-hello` command, including argument handling, so
  that the executable contains no logic.

## Interfaces

- Public header: `include/scada/hello/hello.h`, namespace `scada::hello`.
- No contract; nothing crosses a process boundary.

## How to use it

Link the CMake target `scada::hello`:

```cmake
scada_add_executable(my-tool SOURCES src/main.cpp DEPENDS scada::hello)
```

```cpp
#include "scada/hello/hello.h"

std::cout << scada::hello::greeting() << '\n';
```

## How to test it

```
ctest --preset dev -R scada-hello-unit-test
```
