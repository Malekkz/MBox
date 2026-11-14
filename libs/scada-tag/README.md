# libscada-tag

The client library of the tag database (reference section 5). All access to
the shared-memory data plane goes through it. So far it holds the binary
layout, the tag-set definition and the builder of the `header` region;
attach, read, write, rings and queues follow in later phase 1 issues.

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
- Defines the tag-set definition (`TagSet`: processes, owners, queues, tags)
  and validates it against the contract, sections 1, 2, 5 and 14.
- Computes `config_hash`, the SHA-256 of a tag set, independent of the order
  of its entries.
- Builds the complete image of the `header` region in memory (contract,
  section 5): tables, sorted directory, slot tables, name pool.
- Reads the temporary tag-set file of phase 1 (JSON).

It does not yet map or read any memory, and it writes no file. The values of
`EventRecord.kind` are not defined: the contract names them without numbers.

## Interfaces

- Contract: `docs/contracts/tag-database.md`, version 0.3, layout version 1.
- Public headers in `include/scada/tag/`, namespace `scada::tag`:

  | Header | Content |
  | --- | --- |
  | `types.h` | Tag type, quality, quality flags, write request and result enumerations |
  | `handle.h` | Tag handles |
  | `tag_set.h` | `TagSet`, the limits of the contract, `validate`, `config_hash` |
  | `header_builder.h` | `build_header`: a `TagSet` to the bytes of the `header` region |
  | `tag_set_json.h` | `parse_tag_set_json`: the temporary file format to a `TagSet` |

- Private headers in `src/`: `layout.h` (the structures, their checks and the
  enumerations only the layout uses), `name_hash.h`, `sha256.h` and
  `resolved_tag_set.h`. No code outside this library may include them
  (decision D-25 in `docs/decisions.md`).

`TagSet` is the one interface between configuration and the data plane. The
JSON file is temporary: phase 2 replaces `tag_set_json.h` with the real
configuration, which fills the same `TagSet`.

## Tag-set rules

`validate`, `config_hash` and `build_header` reject a tag set, with a status
and a sentence that names the entry, when:

| Rule | Status code |
| --- | --- |
| A name is empty, too long (127 bytes for tags, 31 bytes for processes, owners and queues) or has a character outside `[a-z0-9_.-]` | `INVALID_ARGUMENT` |
| Two processes, two owners, two queues or two tags have the same name | `ALREADY_EXISTS` |
| A tag names an owner, or an owner or queue names a process, that the tag set does not have | `NOT_FOUND` |
| More than 1,024 processes, 1,023 owners, 4,096 queues or 1,000,000 tags | `OUT_OF_RANGE` |
| A change ring capacity is not a power of two from 1,024 to 1,048,576, or is less than twice the owner's tag count | `OUT_OF_RANGE` |
| The only owner of a process is not named like the process, or one of several is not named `<process>.<writer>` | `INVALID_ARGUMENT` |
| A queue name does not start with `events.`, `cmd.` or `result.`, or its capacity is not a power of two | `INVALID_ARGUMENT` |
| `retain_now` without `retain`; `writable_by_command` on a string tag; a uid of 4,294,967,295; a heartbeat time of 0 ms | `INVALID_ARGUMENT` |

The limit of 4,194,303 tags per owner cannot be reached within 1,000,000
tags.

## Order independence

The order of the entries in a `TagSet` has no effect:

- "Sorted by name" means by the bytes of the name, as unsigned values.
- Processes, owners and queues are sorted by name. The position of a process
  is its process index, of an owner its owner index, of a queue its queue id.
- The slots of an owner are its tags sorted by name.
- The directory is sorted by `(name_hash, name)`; the name pool holds the
  names in directory order, without terminators.

`config_hash` is the SHA-256 of this byte sequence (integers little-endian;
text as a 32-bit length followed by its bytes; lists in name order, each
preceded by its 32-bit count):

```
text "scada-tag-set", u32 1 (encoding version), u32 heartbeat_check_ms
processes: text name, u32 uid, u32 heartbeat_timeout_ms, u32 flags (bit 0 REALTIME)
owners:    text name, text process, u32 change_ring_capacity
queues:    text name, text producer, text consumer, u32 capacity
tags:      text name, text owner, u8 type, u8 flags (as in DirectoryEntry)
```

Generation and creation time are in the header but not in the hash.

## Temporary tag-set file

One JSON document. Lists and entries may be in any order.

```json
{
  "format": 1,
  "heartbeat_check_ms": 250,
  "processes": [
    {"name": "io", "uid": 991, "heartbeat_timeout_ms": 500, "realtime": true},
    {"name": "control", "uid": 992, "realtime": true},
    {"name": "journal", "uid": 993}
  ],
  "owners": [
    {"name": "io", "process": "io", "change_ring_capacity": 1024},
    {"name": "control.fast", "process": "control"},
    {"name": "control.slow", "process": "control"}
  ],
  "queues": [
    {"name": "events.control.fast", "producer": "control", "consumer": "journal", "capacity": 4096},
    {"name": "cmd.io.control.fast", "producer": "control", "consumer": "io"}
  ],
  "tags": [
    {"name": "plant.pump1.speed", "owner": "io", "type": "float64", "writable_by_command": true},
    {"name": "plant.pump1.mode", "owner": "control.slow", "type": "int64", "retain": true, "retain_now": true}
  ]
}
```

| Key | Required | Default | Meaning |
| --- | --- | --- | --- |
| `format` | yes | | Version of this file format; must be 1 |
| `heartbeat_check_ms` | no | 250 | How often `scada-tagd` checks heartbeats, in ms |
| `processes[].name`, `uid` | yes | | Process name and the numeric user id it runs as |
| `processes[].heartbeat_timeout_ms` | no | 3,000; 500 if `realtime` | Heartbeat timeout in ms |
| `processes[].realtime` | no | `false` | The process is a real-time process |
| `owners[].name`, `process` | yes | | Owner name and the process it is a thread of |
| `owners[].change_ring_capacity` | no | 65,536 | Entries of the owner's change ring |
| `queues[].name`, `producer`, `consumer` | yes | | Queue name; the processes that post and read |
| `queues[].capacity` | no | 1,024 | Records |
| `tags[].name`, `owner`, `type` | yes | | Type is `bool`, `int64`, `uint64`, `float64` or `string` |
| `tags[].retain`, `writable_by_command`, `retain_now` | no | `false` | Flags of the tag |

The parser accepts a subset of JSON, enough for this format:

- Strings hold printable ASCII, at most 127 bytes, without escape sequences.
- Numbers are whole numbers from 0 to 4,294,967,295.
- An unknown key, a key that appears twice and a missing required key are
  errors. An error is reported as `line <l>, column <c>: <what>`.
- A document is at most 256 MiB; a list is at most as long as the contract
  allows.

## How to use it

Link the CMake target `scada::tag`:

```cmake
scada_add_library(scada-example SOURCES src/example.cpp DEPENDS scada::tag)
```

```cpp
#include "scada/tag/handle.h"
#include "scada/tag/header_builder.h"
#include "scada/tag/tag_set_json.h"
#include "scada/tag/types.h"

const auto parts = scada::tag::unpack_handle(handle);   // nothing if invalid
const auto type = scada::tag::tag_type_from_raw(raw);   // nothing if unknown

std::string detail;   // filled with the reason when a call fails
const auto tag_set = scada::tag::parse_tag_set_json(text, detail);
if (tag_set.ok()) {
    const auto image = scada::tag::build_header(
        tag_set.value(), {.generation = 1, .created_time_ns = now}, detail);
    // image.value().bytes is the content of the `header` file, with `ready`
    // 0 and state BUILDING; image.value().config_hash is its config hash.
}
```

## How to test it

```
ctest --preset dev -R scada-tag-unit-test
ctest --preset dev -R tag_set_json_fuzz-corpus
cmake --preset perf && cmake --build --preset perf
ctest --preset perf -R scada-tag-header-perf-test
```

- `Conformance01Layout.*` is conformance test 1 of the contract: for each
  structure, the fields in the order and with the sizes of the contract must
  follow one another without a gap and fill the structure.
- `SharedAtomics.*` checks that every field marked `A` in the contract has a
  type that is always lock-free on the target and sits at an address aligned
  for atomic access.
- `Handle.*`, `NameHash.*` and `Types.*` cover handles, the name hash and the
  enumerations, including values that must be rejected.
- `Sha256.*` checks the hash against the test vectors of the standard.
- `TagSetValidate.*` covers every rule in "Tag-set rules", each at its limit.
  `TagSetHash.*` checks the hash against a value computed separately from
  the encoding above, that it is the same for any order, and that every
  field changes it.
- `HeaderBuilder.*` reads the image back through the layout structures: both
  headers, the tables, the directory order, handles, slot tables and name
  pool, and that any order of the entries gives the same bytes.
- `TagSetJson.*` covers the file format, its defaults, and malformed input
  with the reported line and column.
- `tag_set_json_fuzz` (in `fuzz/`) feeds the parser, and the builder behind
  it, with arbitrary bytes. Its seed corpus runs as the unit test
  `tag_set_json_fuzz-corpus` in every build.
- `scada-tag-header-perf-test` (in `tests/perf/`) fails if the header of
  50,000 tags across 10 owners takes more than 1 second to build, and
  reports the time for the largest tag set of 1,000,000 tags.

The tests need no fixture.
