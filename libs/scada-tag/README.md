# libscada-tag

The client library of the tag database (reference section 5). All access to
the shared-memory data plane goes through it. So far it holds the binary
layout only; attach, read, write, rings and queues follow in later phase 1
issues.

## What it does

- Defines every structure of the tag database contract, sections 4 to 10, and
  proves at compile time that size, alignment and every field offset match
  the contract and that no structure has padding.
- Proves at compile time that every field shared through `std::atomic_ref`
  is lock-free, and that the target is 64-bit little-endian.
- Defines the enumerations of the contract, with conversions from the stored
  integer that reject a value that is not an enumerator.
- Packs and unpacks tag handles (10 bits owner index, 22 bits slot).
- Computes the 64-bit FNV-1a hash of a tag name.

It does not yet map or read any memory. The values of `EventRecord.kind` are
not defined: the contract names them without numbers.

## Interfaces

- Contract: `docs/contracts/tag-database.md`, version 0.3, layout version 1.
- Public headers in `include/scada/tag/`, namespace `scada::tag`:
  `types.h` (tag type, quality, quality flags, write request and result
  enumerations) and `handle.h`.
- Private headers in `src/`: `layout.h` (the structures, their checks and the
  enumerations only the layout uses) and `name_hash.h`. No code outside this
  library may include them (decision D-25 in `docs/decisions.md`).

## How to use it

Link the CMake target `scada::tag`:

```cmake
scada_add_library(scada-example SOURCES src/example.cpp DEPENDS scada::tag)
```

```cpp
#include "scada/tag/handle.h"
#include "scada/tag/types.h"

const auto parts = scada::tag::unpack_handle(handle);   // nothing if invalid
const auto type = scada::tag::tag_type_from_raw(raw);   // nothing if unknown
```

## How to test it

```
ctest --preset dev -R scada-tag-unit-test
```

- `Conformance01Layout.*` is conformance test 1 of the contract: for each
  structure, the fields in the order and with the sizes of the contract must
  follow one another without a gap and fill the structure.
- `SharedAtomics.*` checks that every field marked `A` in the contract has a
  type that is always lock-free on the target and sits at an address aligned
  for atomic access.
- `Handle.*`, `NameHash.*` and `Types.*` cover handles, the name hash and the
  enumerations, including values that must be rejected.

The tests need no fixture.
