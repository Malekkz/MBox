# libscada-tag

The client library of the tag database (reference section 5). All access to
the shared-memory data plane goes through it. So far it holds the binary
layout, the tag-set definition, the builder of the `header` region, the side
of `scada-tagd` (generations, the `proc` region and liveness) and attach: a
process joins the current generation, creates or reuses and repairs its own
regions, and maps the regions of other processes read-only; and tag read
and write with the sequence lock; change rings; and reliable queues.

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
- Tags, in the same header: `scada_tag_resolve`, `scada_tag_info`,
  `scada_tag_read`, `scada_tag_read_many`, `scada_tag_write`,
  `scada_tag_write_many`, `scada_tag_read_string` and
  `scada_tag_write_string`. See "Tag read and write" below.
- Observer contexts, in the same header: `scada_tag_observe` and
  `scada_tag_observe_opts`, for read-only tools; and the listing functions
  `scada_owner_info`, `scada_tag_handle`, `scada_proc_resolve` and
  `scada_proc_status`. See "Observe" below.
- Change rings and waking, in the same header: `scada_tag_notify`,
  `scada_sub_open`, `scada_sub_next`, `scada_sub_stats`, `scada_sub_close`
  and the wait sets `scada_waitset_*`. See "Change rings and waking" below.
- Reliable queues, in the same header: `scada_queue_open`,
  `scada_queue_post`, `scada_queue_next`, `scada_queue_ack`,
  `scada_queue_stats`, `scada_queue_close`, `scada_waitset_add_queue`, and
  the records
  `scada_event_record`, `scada_write_request` and `scada_write_result`. See
  "Reliable queues" below.

## Interfaces

- Contract: `docs/contracts/tag-database.md`, version 0.13, layout version 1.
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
  | `scada_tag.h` | The C interface: return codes, attach, detach, heartbeat, notify, status, the tag functions with `scada_value`, subscriptions with `scada_change`, and wait sets. Usable from C and C++ |

- Private headers in `src/`: `layout.h` (the structures, their checks and the
  enumerations only the layout uses), `name_hash.h`, `sha256.h`,
  `resolved_tag_set.h`, `shared_memory.h` (the reviewed helpers that place
  the structures on mapped memory), `region_file.h` (files, directories and
  their checks), `proc_region.h` (the `proc` region), `own_region.h` (a
  region the process writes: found and reused, or created), `header_view.h`
  (the `header` region as an attaching process reads and validates it),
  `peer_regions.h` (regions of other processes, mapped read-only),
  `attachment.h` (attach and the tag functions; the C interface is a thin
  layer over it), `tag_record.h` (the sequence lock of a tag record),
  `utf8.h` (the check of text for a string slot), `clock_check.h`
  (whether reading the clocks is a system call), `change_ring.h` (the
  protocol of a change ring), `futex.h` (waiting and waking on shared
  words), `thread_mark.h` (what tells threads apart), `subscription.h` and
  `wait_set.h` (the classes under `scada_sub` and `scada_waitset`).
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
  (`heartbeat stale`), or further than the timeout ahead of the checker's
  clock (also `heartbeat stale`). A heartbeat exactly as old as the timeout
  is not down.
- The `down` flag in the process table is set or cleared to match, and each
  change is returned to the caller. `scada-tagd` itself is never marked.
  A new generation starts with the flag set for every other process, so a
  process that attached before the first check is returned as up (D-38).
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
6. The queues (contract 9.2): if `proc.<process>` was created, the cursor
   of every queue the process consumes is set to the end of the queue, and
   the records passed over are counted. Every request still in a `cmd`
   queue of the process is answered ABORTED and passed over, unexecuted.
7. The regions are brought into memory, the state becomes RUNNING and the
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
(`PeerRegions`; the callers are tag resolve, subscriptions, wait sets,
attach and queue open for the queues, and the liveness of `scada-tagd` in
`scada_tag_status`). A region that is missing, not ready or fails a check
of contract 3.1 is not an error: it reads as missing and is looked for
again at most once a second, from `scada_tag_notify`, `scada_tag_status`,
and a subscription, queue or wait set that waits on it. Two processes that
start together therefore find each other within about a second: a post
gets `SCADA_E_PEER_DOWN` until the producer has looked again for the
`proc` region of a consumer that attached after it. A
mapping is never removed while the process is attached, so a reader that
holds it keeps a valid view; a region whose file is replaced more than 16
times reads as missing until the reader attaches again.

Attach also reads the tag directory through the slot table of every owner
and keeps the type of every tag, 1 byte each; a slot without a valid entry
fails attach with `SCADA_E_LAYOUT`. And it checks the clocks, see "Tag read
and write".

Not part of attach yet: retained values (issue P1-09).

## Tag read and write

Contract sections 6, 7 and 13; decision D-29.

| Function | Real-time safe | What it does |
| --- | --- | --- |
| `scada_tag_resolve` | no | Name to handle. Maps the regions of the owner read-only, so that the first read makes no system call |
| `scada_tag_info` | no | Name, type, flags, owner and whether the caller owns the tag |
| `scada_tag_read` | yes | One tag into a `scada_value` |
| `scada_tag_read_many` | yes | Several tags with one reading of the clock, and a status for each |
| `scada_tag_write` | yes | One tag; owner thread only |
| `scada_tag_write_many` | yes | Several tags with one pair of clock readings; all or nothing |
| `scada_tag_read_string` | no | The text of a STRING tag with the quality and times of the same write; `buf` NULL with `cap` 0 gives the length alone |
| `scada_tag_write_string` | no | Text of at most 250 bytes of well-formed UTF-8; owner thread only |

A `scada_value` is 48 bytes: type, quality, quality flags, change count,
the value as a 64-bit pattern (BOOL 0 or 1, INT64 and UINT64 the number,
FLOAT64 the bits of a `double`, STRING the number of its string slot),
source time, update time (`CLOCK_REALTIME`), update time
(`CLOCK_MONOTONIC`) and age in ns.

The sequence lock is the one of contract 6.1 and 6.2. A write makes `seq`
odd, stores the fields and makes it even. A read copies the fields between
two loads of `seq` and uses the copy only if both loads gave the same even
number; it tries 64 times. A string slot is written and copied inside the
same window as its record (contract 7). Every field is accessed through
`std::atomic_ref`, so no access is a data race and ThreadSanitizer needs no
suppression. ThreadSanitizer does not check the order of the accesses; the
tests with derived values sample it, and the model check of conformance
test 4 checks it for the Armv8 memory model, together with a check that the
ARM64 library compiles to that order (`tests/model/README.md`).

What a read returns:

| Situation | Return code | Value |
| --- | --- | --- |
| Owner running, record between writes | `SCADA_OK` | What the owner wrote |
| Never written | `SCADA_OK` | Quality BAD, flag NOT_INIT, times 0 |
| Owner down, record between writes | `SCADA_OK` | The last value, quality BAD, OWNER_DOWN added |
| Owner down, record left in the middle of a write | `SCADA_OK` | Value 0, times 0, quality BAD, OWNER_DOWN |
| Region of the owner missing, after a tag of the owner was resolved in this context | `SCADA_OK` | Value 0, times 0, quality BAD, OWNER_DOWN |
| Owner running and mid-write at all 64 attempts | `SCADA_E_BUSY` | Output unchanged; keep the previous value |
| Record does not hold a value of the type of the tag, or holds OWNER_DOWN | `SCADA_E_LAYOUT` | Type of the tag, value 0, times 0, quality BAD |
| No such handle in this generation, or no tag of its owner (another process) was resolved in this context | `SCADA_E_INVALID` | Type 0, value 0, times 0, quality BAD |

Only `SCADA_E_BUSY` leaves the output unchanged. Where value and times are
0 there is no value: a consumer must not plot, compare or act on them.

A process that reads tags of other processes calls `scada_tag_notify` (or
`scada_tag_status`) about once a second from a thread that is not
real-time, also if it writes nothing. Those are the calls that look again
for regions that were missing, so without them an owner that starts after
the reader keeps reading as down. They also repeat the clock check.

An owner is down when its process is: its `proc` region is missing, its
state is not RUNNING, or its heartbeat is older than its timeout (contract
10.1). The library evaluates this at every read, so a clean stop shows at
once and a kill within the heartbeat timeout, with or without `scada-tagd`.
The `down` flag `scada-tagd` keeps is not used for it. `age_ns` is the time
since the update and never negative.

What a write refuses:

| Situation | Return code |
| --- | --- |
| The caller's process does not own the tag | `SCADA_E_NOT_OWNER` |
| `type` is not the type of the tag; `scada_tag_write` on a STRING tag; a string function on a tag that is no STRING | `SCADA_E_TYPE` |
| A quality or quality flag that is none; the flag OWNER_DOWN, which only a read adds; a BOOL that is neither 0 nor 1; text that is not UTF-8 or longer than 250 bytes | `SCADA_E_INVALID` |

A write sets both update times. With `source_time_ns` 0 the source time is
the update time and LOCAL_TIME is set. `change_count` goes up by 1 when
value, quality or quality flags change; for a STRING tag the value is its
text.

All writes to the tags of one owner come from one thread, the first that
writes them; the library records it, so that `scada_tag_notify` knows the
rings of the calling thread. A library built without `NDEBUG` (the `dev`
and sanitizer builds) stops the process when another thread writes them
(contract 13.2).

The real-time safe functions allocate nothing, take no lock and make no
system call, as long as the clocks can be read from the vDSO. Attach checks
that: `scada_status.clock_syscall` is 1 if the kernel's clock source is not
one the vDSO reads, or one clock read through the library takes more than
half the time of a raw `clock_gettime` system call. The check is
repeated at most once a minute from `scada_tag_notify` and
`scada_tag_status`. In the retry loop of a read the processor is told to
pause (`scada::cpu_relax`): a read that finds a write in progress gives up
with `SCADA_E_BUSY` after about 0.7 us on ARM64 and 1 us to 1.6 us on
x86-64, longer than a write of a running owner takes. The rate of
`SCADA_E_BUSY` at the reference load is in `docs/reports/phase-1.md`
(decision D-31). `scada-tag-seqlock-bench` and `scripts/measure-seqlock.sh`
measure it again on another machine.

## Observe

Contract section 11.4; decision D-42.

`scada_tag_observe(&ctx)` gives a context that reads the database without
being a process of the tag set, for tools such as `scadactl`. It makes the
checks of step 1 of attach and reads the tag directory, and nothing else:

- no entry in the process table and no uid check; process index
  `SCADA_NO_PROCESS`;
- no file created, no lock, nothing written to shared memory; every region
  mapped read-only when it is asked for, through `PeerRegions`;
- writes and `scada_queue_open` give `SCADA_E_NOT_OWNER`, so an observer
  has no queue; `scada_tag_heartbeat` and `scada_tag_notify` give
  `SCADA_E_INVALID`;
- any number of observers beside the processes of the generation.

Who may observe is the file permissions: a member of group `scada`, or
root. A caller that may not open the base or generation directory gets
`SCADA_E_PERMISSION` (attach too).

An observer calls `scada_tag_status` about once a second. It looks again
for missing regions and repeats the clock check, and it returns
`SCADA_E_RETIRING` when the generation is RETIRING or, at a look at most
once a second, `current` names another generation, the base directory
under its path is another directory (the database was set up again), or
neither could be read at two looks in a row. The observer then
detaches, observes again and resolves its handles again.

| Function | Real-time safe | What it does |
| --- | --- | --- |
| `scada_owner_info` | no | Name, tag count, ring capacity and process of an owner index |
| `scada_tag_handle` | yes | The handle of `(owner index, slot)`; no context |
| `scada_proc_resolve` | no | Process name to index; maps its `proc` region |
| `scada_proc_status` | yes | State, liveness and its reason, the `down` flag of `scada-tagd`, heartbeat counter and age, `cycle_seq`, pid, uid. `SCADA_E_INVALID` with reason `SCADA_DOWN_NOT_RESOLVED` for a process not resolved in the context, with the fields of the process table filled |

`scada_status.owner_count` and `process_count` give the ranges of the
indexes. Every tag: for each owner, `scada_tag_info` of
`scada_tag_handle(owner, slot)` for each slot below its `tag_count`.

## Change rings and waking

Contract section 8; decision D-32.

A write that changes value, quality or quality flags (for a STRING tag:
its text) appends one change entry to the ring of its owner, after the
record is whole again. The append is real-time safe: no system call. The
entry holds the stored fields: the source time the record got, LOCAL_TIME
included, and the same `update_mono_ns`.

`scada_tag_notify` wakes the consumers: for every ring the calling thread
wrote since its previous call, one `FUTEX_WAKE` on the low 32 bits of the
ring's `write_seq`; then it increments the process's `cycle_seq` and wakes
its waiters. A real-time task calls it once at the end of a cycle; other
code after each batch of writes. It also looks again for missing regions
and repeats the clock check, as `scada_tag_status` does.

| Function | What it does |
| --- | --- |
| `scada_sub_open` | Subscribes to the ring of one owner, from its end on. A ring that is not there yet is no error |
| `scada_sub_next` | The next change, waiting up to a timeout; `SCADA_E_GAP` when changes were lost |
| `scada_sub_stats` | Changes received, gaps, entries lost to overruns, entries refused, the epoch, owner changes, whether the owner is down, and why the last gap came |
| `scada_sub_close` | Closes it, and takes it out of its wait set |
| `scada_waitset_create`, `_add_sub`, `_add_cycle` | Up to 127 subscriptions and cycle counters |
| `scada_waitset_wait` | Waits for any of them with one `futex_waitv` |
| `scada_waitset_fd` | An eventfd for `poll` and `epoll`; starts one thread for the wait set |
| `scada_waitset_destroy` | Stops the thread, leaves the subscriptions open |

A consumer waits on the futex word of a ring through its read-only
mapping; a wake through the owner's read-write mapping reaches it, because
the kernel keys a shared futex by file and offset (the experiment of issue
P1-07, decision D-32). Every wait looks at the ring again at least every
100 ms, so a process that never calls `scada_tag_notify` still delivers
within 100 ms.

`scada_sub_next` returns `SCADA_E_GAP` once for each of these: the
subscription fell `capacity` entries behind (contract 8.2), the owner
attached again (the epoch changed), the ring appeared, disappeared or was
replaced, or the owner's process went down or came back (decision D-33). The subscription then continues at the end of the ring; the
consumer reads the tags it cares about and drops any later change older
than what it read. A change entry is input: one that is not a valid change
of a tag of the owner gives `SCADA_E_LAYOUT` and is skipped.

An owner that dies writes no change; the gap when its process is seen
down tells the consumer, which then reads the tags and gets BAD with
OWNER_DOWN. A waiting consumer gets that gap within the owner's heartbeat
timeout plus 100 ms, at once after a clean stop. While the owner is down, a
change that still arrives comes as BAD with OWNER_DOWN, as a read would
give it. `scada_sub_stats` counts these owner changes.

## Reliable queues

Contract section 9; decision D-40.

A queue has one producing process and one consuming process, fixed in the
queue table (the tag-set builder refuses one process at both ends), and in
each one thread. `scada_queue_open` opens the side the calling process has;
a second open of the same side in one context gets `SCADA_E_BUSY`.

| Function | What it does |
| --- | --- |
| `scada_queue_post` | Producer. Copies one record into the queue; real-time safe |
| `scada_queue_next` | Consumer. The next record, waiting up to a timeout |
| `scada_queue_ack` | Consumer of EVENTS and RESULT: the record is processed |
| `scada_queue_stats` | The counts of one side in its context since attach: posted, delivered, expired, aborted, skipped, answers dropped, invalid, out of range, replaced |
| `scada_queue_close` | Closes the side, and takes it out of its wait set |
| `scada_waitset_add_queue` | The consumer's side as a source of a wait set |

A post (contract 9.1) is refused with `SCADA_E_PEER_DOWN` when the
consumer's process is down by the three rules of contract 10.1, judged by
the producer, or its cursor is out of range; with `SCADA_E_FULL` when
`capacity` records are not yet finished with; and with `SCADA_E_INVALID`
when the record fails its checks. The library sets the id: `seq` of an
event and `request_id` of a request are taken from `write_seq`, which
survives a producer restart, so no id is reused in a generation; it also
sets `posted_mono_ns` of a request to the time of the post (a guard against
a caller's bug, not a security boundary: the consumer bounds the lifetime on
its own clock). One thread posts to a queue, in every build: a second one
is refused with `SCADA_E_INVALID` (a debug build stops). Only the thread
that holds a queue lets go of it, when it closes its side. A post wakes
nobody; `scada_tag_notify` wakes the consumers of the queues the calling
thread posted to.

The checks of a record, at post and again when it is taken, since the
consumer reads it from another process:

| Record | Checks |
| --- | --- |
| Event | `kind` 1 ALARM, 2 COMMAND, 3 AUDIT or 4 SYSTEM; `payload_len` at most 232 |
| Write request | `operation` and `source` of their enumerations; `value_type` BOOL, INT64, UINT64 or FLOAT64, or 0 with value 0 for CANCEL and READ_NOW; a BOOL value 0 or 1; `user_id` not 0 for OPERATOR (the commanding user) and 0 for AUTO and REMOTE; `select_request_id` set for OPERATE and CANCEL only; reserved fields 0; not expired (below) |
| Write result | `status` 1 to 7; reserved fields 0 |

When a record is taken its id must also be its position. A record that
fails is passed over with `SCADA_E_LAYOUT`. A cursor out of range is not
followed: `scada_queue_next` gives `SCADA_E_LAYOUT` and the producer
`SCADA_E_PEER_DOWN` until the next generation; each look is counted in
`out_of_range`. On such a stopped queue `scada_queue_next` waits out its
timeout before it returns `SCADA_E_LAYOUT`, and a wait set sleeps in slices
of 100 ms, so neither spins. The library does not log it: the process logs
once when it starts.

Delivery (contract 9.2):

- EVENTS and RESULT, at least once: the cursor in the consumer's `proc`
  region moves on at `scada_queue_ack`. Until then `scada_queue_next` gives
  the same record again, also after a crash and restart.
- CMD, at most once: the cursor moves on before `scada_queue_next` returns,
  so a request is never delivered twice. A request is not returned, but
  answered EXPIRED in the result queue of the same `<driver>.<source>`,
  when, on the consumer's clock read after `write_seq`, its expiry is not
  after now, it was posted after now, it expires before it was posted, it
  lives longer than 60 s, or its expiry is more than 60 s ahead. The
  cursor moves on first. The thread that takes the requests of a `cmd`
  queue is the one that posts to its result queue (bound at the first
  `scada_queue_next`). At attach, every request still queued is answered
  ABORTED (step 6 of "Attach"). An answer that cannot be posted (result
  queue full, requester down, no result queue, another thread bound) is
  dropped and counted; the requester sees the request expire. One call
  passes over at most a queue's capacity of expired requests before it
  looks at its deadline; with timeout 0 it makes no system call. A driver
  checks the expiry again right before it acts on the device.

A consumer that creates its `proc` region in an existing generation starts
every cursor at the end of its queue and reports the records it skipped in
`skipped` of `scada_queue_stats` (for an events queue: lost audit records).
A queue whose file is replaced, removed or shrunk inside the generation is
dead: the consumer remembers the first mapping it saw, gets `SCADA_E_LAYOUT`
when it finds another or none, counts `replaced`, and stores an all-ones
cursor so that the producer gets `SCADA_E_PEER_DOWN`. It stays so until a
tag-set change or a reboot (decision D-40, issue #53). Two gaps remain until
#53 adds a base sequence: a post made between the recreation and the
consumer's next look (up to about 1 s) returns `SCADA_OK` and the record is
lost, which on an event queue is an action without an audit record; and a
consumer that was not attached during the recreation cannot tell the new
file from the old one, so it may deliver records whose ids repeat.

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

A process attaches, writes its own tags and reads those of others like
this:

```c
#include "scada/tag/scada_tag.h"

scada_ctx *ctx = NULL;
int code = scada_tag_attach("modbus", &ctx);   /* SCADA_OK or SCADA_E_... */

uint32_t flow, setpoint;                       /* once, not in the cycle */
scada_tag_resolve(ctx, "plant.pump1.flow", &flow);          /* own tag */
scada_tag_resolve(ctx, "plant.pump1.setpoint", &setpoint);  /* of another */

/* in the cycle; both calls are real-time safe */
scada_value in;
if (scada_tag_read(ctx, setpoint, &in) == SCADA_OK &&
    in.quality == SCADA_QUALITY_GOOD) { /* use in.value */ }
scada_value out = {.type = SCADA_TYPE_FLOAT64, .quality = SCADA_QUALITY_GOOD};
memcpy(&out.value, &measured, sizeof out.value);   /* the bits of a double */
scada_tag_write(ctx, flow, &out);

/* in the main loop, at most every third of the heartbeat timeout: */
if (scada_tag_heartbeat(ctx) == SCADA_E_RETIRING) { /* detach, attach again */ }
scada_tag_detach(ctx);
```

## How to test it

```
ctest --preset dev -R scada-tag-unit-test
ctest --preset dev -R scada-tagd-integration-test
ctest --preset dev -R scada-tag-access-integration-test
ctest --preset dev -R scada-tag-rt-integration-test
ctest --preset dev -R scada-tag-change-ring-integration-test
ctest --preset dev -R scada-tag-queue-integration-test
ctest --preset dev -R scada-tag-observer-integration-test
sudo ctest --preset root
ctest --preset dev -R 'tag_set_json_fuzz-corpus|region_name_fuzz-corpus'
ctest --preset dev -R 'header_region_fuzz-corpus|utf8_fuzz-corpus'
ctest --preset dev -R queue_record_fuzz-corpus
cmake --preset perf && cmake --build --preset perf
ctest --preset perf -R scada-tag-header-perf-test
ctest --preset perf -R scada-tag-wake-perf-test
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
  `SCADA_E_GAP` of test 7 is tested with the change rings.
- `scada-tag-test-peer` (in `tests/peer/`) is a helper for integration
  tests: it attaches as one process through the C interface and writes its
  heartbeat until it is stopped or killed. With `mid-write` it leaves a tag
  record with an odd sequence number, as a killed writer does.
- `TagRecordLock.*` covers the sequence lock on records in plain memory:
  what a write stores, the sequence number, the change count, and a record
  left in the middle of a write.
- `TagAccess.*` covers the tag functions as an owner and a reader see them:
  every row of the two tables in "Tag read and write", strings, and records
  that are not valid. `ClockCheck.*` covers the rule of the clock check.
- `TagAccess.Conformance03*` and `Conformance23*` run a writer at its
  maximum rate against readers in other threads, with values and texts
  whose fields are derived from one another; `Conformance05*`, `06*` and
  `08*` are the part of those conformance tests that needs one process.
- `ChangeRing.*` covers the ring protocol on memory of the test: order,
  nothing after the last entry, and a gap exactly at `capacity` entries
  behind. `Futex.*` is the experiment of issue P1-07: a wait on a read-only
  mapping of a file is woken through a read-write mapping of it, alone and
  with `futex_waitv` over several words; a changed word and a deadline end
  a wait.
- `Changes.*` covers the rings as owners and consumers use them: what a
  write appends, gaps by overrun, owner restart and a ring that appears,
  entries that are not valid, notify against the 100 ms slice, and wait
  sets with `futex_waitv`, the eventfd and `cycle_seq`. The tests named
  `Conformance07`, `13`, `14` and `24` are those conformance tests in one
  process.
- `scada-tag-change-ring-integration-test` (in `tests/integration/`) runs
  conformance tests 7 (the ring), 13 and 24 with a writer process, which
  `scada-sim` (`libs/scada-sim`) plays with `--notify each`.
- `Observer.*` and `TagHandle.*` cover observer contexts in one process:
  no process entry, every refused call, the generation directory
  unchanged (names, inodes, sizes, modes, owners, times and bytes) after
  observe, resolve, read, subscribe, wait and status, read-only mappings
  only, several observers beside a live attach, process status, listing
  every tag, both kinds of generation change, no database, a missing
  region, a generation that is not running, and a caller who may not read
  the base directory.
- `scada-tag-observer-integration-test` (in `tests/integration/`) runs
  observers against real processes: three observers and an attach beside a
  live writer, `/proc/self/maps` with only read-only mappings, an observer
  started before `scada-tagd`, and a generation change by a restart of
  `scada-tagd` with another tag set.
- `scada-tag-observer-users-test` (in `tests/integration/`, label `root`)
  must run as root (`sudo ctest --preset root`): a child that is user
  65534 hosts the database and a live `io`; root observes; a user in no
  group of the database gets `SCADA_E_PERMISSION` from observe and attach;
  a user in the group but in no entry of the process table observes
  through read-only mappings.
- `scada-tag-wake-perf-test` (in `tests/perf/`) measures the time from a
  write in another process to the return of a blocked `scada_sub_next`. It
  fails on x86-64 if the 99th percentile is 1 ms or more, and prints the
  figures on every target. With `SCADA_PERF_REPORT_ONLY=1` it only prints
  them, as the pipeline runs it. A second case measures the same with 8
  observers waiting on one ring, all woken by one notify.
- `scada-tag-access-integration-test` (in `tests/integration/`) uses real
  processes: `Conformance02*` writes 50,000 tags of 10 owner threads at
  50,000 updates per second against 4 readers, for 3 s, or for
  `SCADA_CONFORMANCE_SECONDS` seconds (600 for the 10 minutes of the
  contract); `Conformance03*` does the same at the maximum rate;
  `Conformance05*` kills an owner in the middle of a write, with and
  without `scada-tagd`; `Conformance06*` stops one cleanly.
- `Queues.*` covers the reliable queues in one process: order, ids, FULL,
  PEER_DOWN, redelivery before the ack and after a restart, the drain of
  the `cmd` queues at attach, expiry, the record checks at post and when a
  record is taken, cursors and `write_seq` out of range, a new `proc`
  region, a queue file replaced or removed inside the generation
  (`ReplacedQueue.*` too), the answers EXPIRED and ABORTED and their drops,
  the thread of a queue, the counts, and wait sets. `QueueRecords.*`
  covers the record checks alone.
  Together they are the part of conformance tests 16 to 19 that needs one
  process.
- `scada-tag-queue-integration-test` (in `tests/integration/`) runs
  conformance tests 16, 17 and 19 with real processes, and one million
  records through one queue of 1,024 places without loss, duplication or
  reordering. The other end is `scada-tag-test-queue` (in
  `tests/queue_peer/`), which posts, consumes, holds a record without its
  ack, or drives a `cmd` queue.
- `scada-tag-rt-integration-test` is conformance test 22: it traces the
  helper `scada-tag-rt-probe` (in `tests/probe/`) and fails if a real-time
  safe function makes a system call, calls a C++ allocation function or
  takes a page fault, or if the library reports that the clocks of the
  machine cannot be read without a system call. It does not see a direct
  call of `malloc`; the banned-function check keeps those out of the
  library. It exists only in a native build without a sanitizer.
- `TagAccessProcesses.ASecondWritingThreadStopsADebugBuild` shows the
  one-thread check of contract 13.2 with a real process.
- The writing side of the load tests is `scada-sim` (`libs/scada-sim`):
  one process with one thread for each owner, writing CHECK values.
  `scada-tag-test-writer` (in `tests/writer/`) only writes a tag from two
  threads, for the test of the one-thread check.
- Conformance test 4, the model check of the sequence lock and the change
  ring, is in `tests/model/`.
- `header_region_fuzz` (in `fuzz/`) feeds the reader of the `header` region
  with arbitrary bytes and then does with the contents what attach, resolve
  and info do.
  Its seed corpus runs as `header_region_fuzz-corpus`.
- `utf8_fuzz` (in `fuzz/`) feeds the check of text for a string slot with
  arbitrary bytes and compares its answer with a decoder written another
  way. Its seed corpus runs as `utf8_fuzz-corpus`.
- `region_name_fuzz` (in `fuzz/`) feeds the parsers of directory entry
  names with arbitrary bytes and checks that they accept only the names this
  library writes. Its seed corpus runs as `region_name_fuzz-corpus`.
- `queue_record_fuzz` (in `fuzz/`) takes arbitrary bytes as an event, a
  write request and a write result, compares the checks with the rules
  written another way, checks the round trip through each structure, and
  the expiry check on any pair of times. Its seed corpus runs as
  `queue_record_fuzz-corpus`.
- `tag_set_json_fuzz` (in `fuzz/`) feeds the parser, and the builder behind
  it, with arbitrary bytes. Its seed corpus runs as the unit test
  `tag_set_json_fuzz-corpus` in every build.
- `scada-tag-header-perf-test` (in `tests/perf/`) fails if the header of
  50,000 tags across 10 owners takes more than 1 second to build, and
  reports the time for the largest tag set of 1,000,000 tags.

The tests need no fixture.
