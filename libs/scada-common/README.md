# libscada-common

Code shared by every component (reference 3.3). For now it holds one header;
issue P0-06 adds status values, results, logging and clocks.

## What it does

- Defines `SCADA_RT`, the annotation that marks a function as real-time
  (`CLAUDE.md`, "Real-time code").
- It does not enforce the real-time rules itself. The `check` target does; see
  `tools/check/README.md`.

## Interfaces

- Public header: `include/scada/common/realtime.h`.
- No contract; nothing crosses a process boundary.

## How to use it

Link the CMake target `scada::common` and mark each real-time function at its
declaration:

```cpp
#include "scada/common/realtime.h"

SCADA_RT Status read(TagHandle tag, Value& value) noexcept;
```

## How to test it

```
ctest --preset dev -R scada-common-unit-test
```
