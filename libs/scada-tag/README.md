# libscada-tag

The client library of the tag database (reference section 5). All access to
the shared-memory data plane goes through it. So far it holds the binary
layout, the tag-set definition, the builder of the `header` region, the side
of `scada-tagd` (generations, the `proc` region and liveness) and attach: a
process joins the current generation, creates or reuses and repairs its own
regions, and maps the regions of other processes read-only. Read, write,
rings and queues follow in later phase 1 issues.

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
- For `scada-tagd` (`GenerationHost`): creates or reuses a generation, owns
  `header` and `proc.tagd`, sets and clears the `down` flag of every process,
  and removes what does not belong in the generation directory. See
  "The side of scada-tagd" below.
- For every other process (`scada/tag/scada_tag.h`, the C interface of
  contract section 13): `scada_tag_attach`, `scada_tag_attach_opts`,
  `scada_tag_detach`, `scada_tag_heartbeat` and `scada_tag_status`. See
  "Attach" below.

It cannot yet attach a process, and it reads and writes no tag. The values
of `EventRecord.kind` are not defined: the contract names them without
numbers.

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
  | `generation_host.h` | `GenerationHost`: the side of `scada-tagd`. Only `scada-tagd` uses it |
  | `liveness.h` | `DownReason`: why a process counts as down |
  | `scada_tag.h` | The C interface: return codes, attach, detach, heartbeat, status. Usable from C and C++ |

- Private headers in `src/`: `layout.h` (the structures, their checks and the
  enumerations only the layout uses), `name_hash.h`, `sha256.h`,
  `resolved_tag_set.h`, `shared_memory.h` (the reviewed helpers that place
  the structures on mapped memory), `region_file.h` (files, directories and
  their checks), `proc_region.h` (the `proc` region), `own_region.h` (a
  region the process writes: found and reused, or created), `header_view.h`
  (the `header` region as an attaching process reads and validates it),
  `peer_regions.h` (regions of other processes, mapped read-only) and
  `attachment.h` (attach itself; the C interface is a thin layer over it).
  No code outside this library may include them (decision D-25 in
  `docs/decisions.md`).

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

## The side of scada-tagd

`GenerationHost::start` does what contract section 11.1 describes; decision
D-27 records the choices the contract left open.

| Step | Rule |
| --- | --- |
| Base directory | Must be a directory owned by the user of `scada-tagd` with mode `2750`. `scada-tagd` locks it; a second instance is refused |
| Tag set | Must hold a process `tagd` with the user id `scada-tagd` runs as |
| Reuse | The generation `current` points to is reused if its directory has mode `3770`, and its `header` is owned by `scada-tagd`, ready, RUNNING, and equal byte for byte to the header the tag set builds (apart from `ready`, `state`, the creation time and the `down` flags) |
| Clean-up | Otherwise every other `gen-<n>` directory in which no process holds the lock on its `proc` region is removed |
| Room | The file system must have room for all regions of the tag set; otherwise the start fails and nothing is created |
| New generation | Number: the highest found plus 1. Directory mode `3770`, files `0640`, set explicitly. `header` is written under `.tmp.header.<pid>`, set ready, renamed, set RUNNING; then `current` is switched by renaming a link over it |
| Old generation | Its state is set to RETIRING |
| `proc.tagd` | Created or reused, locked, RUNNING, first heartbeat |

`GenerationHost::check`, called every `heartbeat_check_ms`:

- A process is down if its `proc` region is missing, fails a check of
  contract 3.1 or is not ready (`region missing`); else if its state is not
  RUNNING (`not running`); else if its heartbeat is older than its timeout
  (`heartbeat stale`). A heartbeat exactly as old as the timeout is not down.
- The `down` flag in the process table is set or cleared to match, and each
  change is returned to the caller. `scada-tagd` itself is never marked.
- Every entry of the generation directory that the header does not list, or
  whose owner is not the user of that region, is removed and returned to the
  caller. A file `.tmp.<region>.<pid>` of a listed region with the right
  owner is a region being created and stays.

A region file is created as contract 3.2 says: under `.tmp.<name>.<pid>`,
mode `0640`, full size allocated at once, then set ready and renamed. The
rename never replaces an existing file. A full file system gives an error
here, not a signal later.

## Attach

`scada_tag_attach(process_name, &ctx)` follows contract section 11.2
(decision D-28):

1. The base directory must be a directory owned by the uid of the user
   `scada-tagd` with mode `2750`. `current` is resolved once. The generation
   directory must be owned by that uid with mode `3770`. `header` is opened
   without following a symbolic link and must be a regular file of that uid.
2. The header is validated before anything in it is used: magic and layout
   version (`SCADA_E_LAYOUT` if they are not this library's), `ready`, kind,
   generation and size, every count against the limits of contract section
   14, every table offset against the file size, and every entry of the
   process, owner and queue tables. The tag directory is checked entry by
   entry when it is used.
3. The process must be in the process table (`SCADA_E_NOT_FOUND`) and run as
   the uid given there (`SCADA_E_PERMISSION`).
4. `proc.<process>` is attached and locked first; a second process of the
   same name gets `SCADA_E_BUSY` and changes nothing. Then, for every owner
   of the process, `tags.<owner>`, `strings.<owner>` (if it has STRING tags)
   and `changes.<owner>`, and every queue the process produces:

   | Found under the name | What attach does |
   | --- | --- |
   | Nothing | Creates the region as contract 3.2 says |
   | A region of the right owner, size, generation and content, ready | Reuses it |
   | Anything else owned by the caller (not ready, wrong size, a symbolic link) | Removes it and creates the region |
   | Anything owned by another user | Fails with `SCADA_E_PERMISSION`; the entry is left for `scada-tagd` |

5. A reused `tags` region is repaired (contract 6.3): every record with an
   odd `seq` gets quality BAD and flag NOT_INIT, its string slot gets length
   0, then `seq + 1` is stored. Other records keep their value. The `epoch`
   of a reused change ring is incremented.
6. The regions are brought into memory, the state becomes RUNNING and the
   first heartbeat is written. If the generation is RETIRING, attach starts
   again from step 1, at most 5 times, then returns `SCADA_E_RETIRING`.

A new `tags` region holds, for every record, `seq` 0, quality BAD, flag
NOT_INIT and all times 0. The value of a STRING record is its string slot:
its rank among the owner's STRING tags in slot order.

`scada_tag_heartbeat` writes the heartbeat and returns `SCADA_E_RETIRING`
when the generation is retiring. It makes no system call. `scada_tag_status`
fills a `scada_status` (generation, its state, the process index, whether
`scada-tagd` is down, and what attach created, reused and repaired) and
returns `SCADA_E_RETIRING` or `SCADA_E_TAGD_DOWN` when that is the case.
`scada_tag_detach` sets the state STOPPING, so the process reads as down at
once; its region files stay for the next attach.

With `SCADA_E_IO`, `errno` holds the error of the operating system call that
failed: `ENOENT` if there is no base directory or no current generation,
`ENOSPC` if the file system has no room for a region.

Regions of other processes are mapped read-only when they are asked for
(`PeerRegions`; the callers are tag resolve, subscriptions and queues of the
next issues, and today the liveness of `scada-tagd` in `scada_tag_status`).
A region that is missing, not ready or fails a check of contract 3.1 is not
an error: it reads as missing and is looked for again at most once a second,
from `scada_tag_status` now and from `scada_tag_notify` once it exists. A
mapping is never removed while the process is attached, so a reader that
holds it keeps a valid view; a region whose file is replaced more than 16
times reads as missing until the reader attaches again.

Not part of attach yet: retained values (issue P1-09), draining the `cmd`
queues and the cursors of a new `proc` region (issue P1-08), the clock check
of contract 13.1 (issue P1-06).

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

A process attaches like this:

```c
#include "scada/tag/scada_tag.h"

scada_ctx *ctx = NULL;
int code = scada_tag_attach("modbus", &ctx);   /* SCADA_OK or SCADA_E_... */
/* in the main loop, at most every third of the heartbeat timeout: */
if (scada_tag_heartbeat(ctx) == SCADA_E_RETIRING) { /* detach, attach again */ }
scada_tag_detach(ctx);
```

## How to test it

```
ctest --preset dev -R scada-tag-unit-test
ctest --preset dev -R scada-tagd-integration-test
ctest --preset dev -R 'tag_set_json_fuzz-corpus|region_name_fuzz-corpus'
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
- `RegionNames.*`, `RegionSizes.*` and `RegionFile.*` cover the names and
  sizes of regions, the base directory check, and creating and opening a
  region: modes under umask 077, no replacement of an existing file, and
  every reason to reject a file (wrong owner, size, header, not ready, a
  symbolic link, a FIFO, a directory).
- `ProcRegion.*` covers the `proc` region: create, reuse, the refusal of a
  second process with the same name, rebuilding an unusable region, and the
  liveness rules at their exact limits.
- `Host.*` covers `GenerationHost`.
- In these three groups, the tests named `Conformance05`, `06`, `09`, `10`,
  `11` and `20` are the part of `scada-tagd` and of region creation in those
  conformance tests of the contract; the part of the reading library follows
  with attach and read.
- These tests create their database in the temporary directory of the
  machine and need no privilege. A user other than the one running the tests
  is simulated by a header that names another user id; nothing runs as that
  user. Conformance test 11, second part, uses a limit on the file size in
  place of a full file system.
- `ReadHeader.*` and `HeaderFile.*` cover the reader of the `header` region:
  what a valid header gives, and 32 ways in which a header is not valid.
- `OwnRegions.*` covers a region the process writes: created, reused, or
  removed and created again; an entry of another user is left.
- `Attach.*` and `TagRecords.*` cover attach: who may attach, the regions
  created and their initial content, reuse, repair, the epoch, and what
  happens to unusable entries under a region name.
- `Peers.*` covers the regions of other processes: mapped on demand, missing
  and unusable ones tolerated, looked for again at most once a second.
- `CApi.*` covers the C interface: arguments, return codes, `errno` and the
  status structure.
- In these groups, the tests named `Conformance07`, `09`, `10`, `11` and
  `12` are the part of attach in those conformance tests of the contract.
  Test 7 lacks `SCADA_E_GAP`, which comes with the change rings (issue
  P1-07).
- `scada-tag-test-peer` (in `tests/peer/`) is a helper for integration
  tests: it attaches as one process through the C interface and writes its
  heartbeat until it is stopped or killed. With `mid-write` it leaves a tag
  record with an odd sequence number, as a killed writer does.
- `header_region_fuzz` (in `fuzz/`) feeds the reader of the `header` region
  with arbitrary bytes and then does with the contents what attach does.
  Its seed corpus runs as `header_region_fuzz-corpus`.
- `region_name_fuzz` (in `fuzz/`) feeds the parsers of directory entry
  names with arbitrary bytes and checks that they accept only the names this
  library writes. Its seed corpus runs as `region_name_fuzz-corpus`.
- `tag_set_json_fuzz` (in `fuzz/`) feeds the parser, and the builder behind
  it, with arbitrary bytes. Its seed corpus runs as the unit test
  `tag_set_json_fuzz-corpus` in every build.
- `scada-tag-header-perf-test` (in `tests/perf/`) fails if the header of
  50,000 tags across 10 owners takes more than 1 second to build, and
  reports the time for the largest tag set of 1,000,000 tags.

The tests need no fixture.
