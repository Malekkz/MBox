# Contract: tag database

| | |
| --- | --- |
| Contract version | 0.18 (draft) |
| Layout version | 1 |
| Status | Accepted for phase 1 implementation (reviews P1-01, section 18). Becomes 1.0 when phase 1 passes its acceptance tests. |
| Implements | Reference architecture, section 5 |
| Owners of this contract | `scada-tagd`, `libscada-tag` |

This contract defines the shared-memory data plane exactly enough that two
independent implementations would interoperate. All access goes through
`libscada-tag`; no other code may depend on the binary layout.

Until this contract reaches 1.0 it may change freely within phase 1. After
that, any change needs a layout version bump and a migration note.

## 1. Conventions

- All integers are little-endian. Only 64-bit little-endian targets are
  supported (x86-64, ARM64).
- Every structure below has a fixed size and alignment, checked with
  `static_assert` in the library. No structure has implicit padding; every gap
  is a named reserved field, written as zero.
- Structures hold plain integers only. A field marked `// A` is shared between
  processes while it changes: the library reads and writes it only through
  `std::atomic_ref`, which must be lock-free on both targets. No
  `std::atomic` object is placed in shared memory.
- A tag value is one `uint64_t` holding a bit pattern: BOOL is 0 or 1, INT64
  and UINT64 are the integer, FLOAT64 is IEEE 754 binary64, STRING is the slot
  number. The library converts with `std::bit_cast` and rejects a BOOL that is
  neither 0 nor 1.
- Enumerations are `enum class` in the library. They are written here with
  their underlying type and values.
- Times are nanoseconds as `int64_t`. `*_time_ns` is `CLOCK_REALTIME` (UTC).
  `*_mono_ns` is `CLOCK_MONOTONIC`. Ages and timeouts always use monotonic time.
- Names are ASCII, lower case, `[a-z0-9_.-]`, at most 127 bytes for tags and
  31 bytes for owners, processes and queues. An owner name does not start
  with `.` (section 12).
- Every offset in a region is a multiple of 8 and is validated against the
  mapped size before use.
- The minimum kernel is Linux 5.16 (`futex_waitv`, section 8.4).

## 2. Terms

| Term | Meaning |
| --- | --- |
| Process | One service, running as its own user, with one `proc` region |
| Owner | One writing thread of one process. Every tag has exactly one owner. A process has one owner per thread that writes tags (for example one per control task, one per driver channel) |
| Region | One file in the database directory, mapped into memory |
| Generation | One complete set of regions built from one configuration |
| Handle | A 32-bit reference to a tag, valid for one generation |
| Change ring | Lossy broadcast buffer of tag changes, one per owner |
| Reliable queue | Lossless queue with one producing thread and one consuming thread |

Owners and queues are declared in the tag-set definition. A process with one
writing thread has one owner, named like the process. Otherwise owners are
named `<process>.<writer>`.

## 3. Directory and files

```
/dev/shm/scada/
  current -> gen-<generation>          symlink, switched atomically
  gen-<generation>/
    header                             written by scada-tagd
    proc.<process>                     written by that process
    tags.<owner>                       written by the owner
    strings.<owner>                    written by the owner (only if it has string tags)
    changes.<owner>                    written by the owner
    events.<producer>                  written by the producer
    cmd.<driver>.<source>              written by the source
    result.<driver>.<source>           written by the driver
```

`<producer>`, `<driver>` and `<source>` are owner names when the process posts
from more than one thread, so that every queue has exactly one producing
thread. The full queue name is at most 31 bytes; the tag-set builder rejects a
longer one.

The base directory and the retain directory (section 12) are the defaults.
`scada_tag_attach_opts` (section 13) can replace both, so tests can run in
parallel and without the service users.

### 3.1 Permissions

- `/dev/shm/scada/` is created at boot by the platform (the service manager's
  temporary-files mechanism), not by `scada-tagd`, owned by `scada-tagd:scada`,
  mode `2750`. Only `scada-tagd` can create a generation or switch `current`.
- `scada-tagd` refuses to start, and a client refuses to attach
  (`SCADA_E_PERMISSION`), unless the base directory is a directory owned by
  the uid of the user `scada-tagd` with exactly that mode. The client looks the
  uid up by name at attach. With a replaced base directory (tests), the
  expected uid is given in the options.
- `gen-<generation>/` is owned by `scada-tagd:scada`, mode `3770` (setgid and
  sticky): group members can create files; a file can be removed only by its
  owner or by `scada-tagd` as owner of the directory.
- Every region file has mode `0640` and group `scada`. Its owner is the user of
  the process that writes it.
- Modes are set with an explicit `fchmod` (or `chmod` for directories) after
  creation, because the creation mode is reduced by the umask.
- A process maps its own regions read-write and all others read-only.
- Read access to the database is membership of group `scada` (or root).
  Any member may open an observer context (section 11.4) without an entry
  in the process table, and read every tag, process and ring. Such reads
  are not audited by the library. Membership also allows creating files in
  `gen-<generation>/` and taking the lock of a `proc` region, which can keep
  a process from attaching (section 3.3); the observer does neither.
- Every file is opened relative to the generation directory with `openat` and
  `O_NOFOLLOW | O_CLOEXEC`, and checked with `fstat` on the descriptor, never
  by path: it must be a regular file, its owner uid must equal the uid recorded
  for that owner or process in the header, its size must equal the size the
  header gives, and the `RegionHeader` fields (magic, layout version, kind,
  generation, size, index) must match. A mismatch is treated as "region
  missing" and reported.
- The `header` file must be owned by the uid of `scada-tagd`.
- A process resolves `current` once at attach and opens every region relative
  to that directory, so a switch of `current` cannot mix two generations.
- `scada-tagd` removes, at each liveness check, any entry in the generation
  directory that the header does not list or whose owner uid is wrong, and
  reports it.

### 3.2 Who creates what

`scada-tagd` creates the generation directory and the `header` region, which
contains the size of every other region. Each process creates its own regions
from those sizes when it attaches. This keeps `scada-tagd` unprivileged: it
never needs to create files on behalf of another user.

A region is created under a temporary name (`.tmp.<name>.<pid>`): create with
`O_CREAT | O_EXCL`, `fchmod`, `ftruncate`, `posix_fallocate` (so a full
`/dev/shm` fails here with an error, not later with a signal), initialise, set
`ready`, then rename into place. A crash therefore never leaves a half-built
region under its real name. At attach a process removes its own stale
temporary files.

### 3.3 Accepted risks

These follow from one user per service inside one trusted group, and are
accepted for layout version 1:

- The owner of a region can shrink its file. Processes that have it mapped are
  then killed by `SIGBUS` on access. A compromised service can in this way stop
  another service, which the supervisor restarts. Preventing it would need
  sealed memory handed out by `scada-tagd`, which contradicts section 17,
  item 3.
- `scada-tagd` maps every queue region read-only for its report (section
  9.2), so a producer that shrinks its queue file in place can stop
  `scada-tagd` by `SIGBUS` too, between its size check and its read. The
  service manager restarts it, and a restart reuses the generation.
- Base sequence (9.1, D-49): ids of a queue file created anew can repeat ids
  of an earlier file, for a consumer that was not attached during the
  recreation, when both the queue file and the producer's `proc` region are
  lost and the consumer's cursor is 0, or when all three regions are lost.
  The first is likelier than it reads: cleanup by name removes a process's
  files together. A consumer that was attached sees it (a base below the old
  `write_seq`) and stops the queue.
- The `proc` lock is an `flock` on one file (section 10). If a running
  process's `proc` file is removed, a second instance can attach beside the
  first; then two producers write, and the old queue file is not final when
  the consumer leaves it. The base guarantees unique ids only while at most
  one instance of the producer lives.
- A shrink of a queue file that was already unlinked cannot be seen by
  name; a consumer that still reads it is stopped by `SIGBUS`.
- A consumer keeps every mapping of a queue name it has seen, up to 16
  (D-28), so locked memory can reach 16 times the queue's size for a
  process under `mlockall`.
- A group member can create a file or directory under a name it does not own.
  The checks above reject it and `scada-tagd` removes it, but the rightful
  process cannot attach until then. A directory that `scada-tagd` cannot remove
  stays until reboot; removal of an old generation is best effort.

### 3.4 Deployment requirements

- Service users are system users. The service manager must not remove their
  shared-memory files at logout (`RemoveIPC` off for them).
- Units must not use a private IPC namespace, private users or dynamic users.
  `ProtectSystem=strict` and `PrivateTmp=yes` are compatible.
- Processes must not run in a private time namespace: heartbeat times are
  compared across processes on `CLOCK_MONOTONIC` (section 10.1), and an
  offset clock would make a process look stale or keep it alive.
- Real-time processes need a locked-memory limit that covers their mappings;
  `scada-tagd` needs write access to the retain directory.
- `fs.protected_hardlinks` and `fs.protected_symlinks` are 1.
- A process feeds the service manager's watchdog from the same loop that
  writes its heartbeat.
- The unit does not suspend, hibernate or hybrid-sleep: the platform
  disables them (`docs/install.md` says how). `CLOCK_MONOTONIC` stays the
  clock of every `*_mono_ns` and does not advance while the system is
  suspended, so after a resume a write request would be executed later in
  real time than its requester allowed, and every heartbeat would still
  look fresh. Whether the platform disabled suspend cannot be checked from
  inside a unit, so `scada-tagd` looks for its traces (D-46): it logs at
  start, once at INFO, the sleep states the kernel offers
  (`/sys/power/state`), and a WARNING when `CLOCK_BOOTTIME` minus
  `CLOCK_MONOTONIC`, the time spent suspended since boot, is above 0 at
  start or grows. A virtual machine that is paused stops both clocks and
  is seen by neither check.

## 4. Region header

Every region starts with this 64-byte header.

```cpp
struct RegionHeader {            // 64 bytes, alignas(64)
  uint64_t magic;                // "SCADARGN" = 0x4E47524144414353
  uint32_t layout_version;       // 1
  uint32_t kind;                 // RegionKind
  uint64_t generation;
  uint64_t size_bytes;           // total file size
  uint16_t owner_index;          // index into the owner or process table
  uint16_t queue_id;             // for queue regions, else 0xFFFF
  uint32_t ready;                // A; 0 while being initialised, 1 when usable
  uint8_t  reserved[24];
};

enum RegionKind : uint32_t {
  HEADER = 1, PROC = 2, TAGS = 3, STRINGS = 4,
  CHANGES = 5, EVENTS = 6, CMD = 7, RESULT = 8
};
```

A reader must not use a region until `ready` is 1. The creator writes `ready`
last.

## 5. The `header` region

Layout, in order: `RegionHeader`, `GlobalHeader`, owner table, process table,
queue table, tag directory, slot tables, name pool. Offsets are from the start
of the file.

```cpp
struct GlobalHeader {            // 128 bytes
  uint32_t state;                // A; 1 BUILDING, 2 RUNNING, 3 RETIRING
  uint32_t tag_count;
  uint16_t owner_count;
  uint16_t process_count;
  uint16_t queue_count;
  uint16_t reserved0;
  uint32_t heartbeat_check_ms;   // how often tagd checks heartbeats
  uint32_t reserved1;
  uint64_t owner_table_offset;
  uint64_t process_table_offset;
  uint64_t queue_table_offset;
  uint64_t directory_offset;
  uint64_t name_pool_offset;
  uint64_t name_pool_size;
  int64_t  created_time_ns;
  uint8_t  config_hash[32];      // SHA-256 of the tag-set definition
  uint8_t  reserved[16];
};

struct OwnerEntry {              // 64 bytes
  char     name[32];             // NUL-padded
  uint32_t uid;
  uint32_t tag_count;            // slots in tags.<owner>
  uint32_t string_slots;         // slots in strings.<owner>, may be 0
  uint32_t change_ring_capacity; // entries, power of two
  uint16_t process_index;        // the process this owner is a thread of
  uint16_t reserved0;
  uint32_t reserved1;
  uint64_t slot_table_offset;    // uint32_t directory index for each slot
};

struct ProcessEntry {            // 64 bytes
  char     name[32];
  uint32_t uid;
  uint32_t heartbeat_timeout_ms;
  uint32_t flags;                // bit 0: REALTIME
  uint32_t down;                 // A; written by scada-tagd only (section 10.1)
  uint8_t  reserved[16];
};

struct QueueEntry {              // 64 bytes
  char     name[32];             // e.g. "events.alarm", "cmd.modbus.control"
  uint16_t queue_id;
  uint16_t kind;                 // 6 EVENTS, 7 CMD, 8 RESULT
  uint16_t producer_process;
  uint16_t consumer_process;
  uint32_t capacity;             // records, power of two
  uint32_t record_size;          // bytes
  uint8_t  reserved[16];
};

struct DirectoryEntry {          // 24 bytes
  uint64_t name_hash;            // FNV-1a 64-bit of the tag name
  uint32_t handle;
  uint32_t name_offset;          // into the name pool
  uint16_t name_len;
  uint8_t  type;                 // TagType
  uint8_t  flags;                // bit 0 RETAIN, bit 1 WRITABLE_BY_COMMAND,
                                 // bit 2 RETAIN_NOW (section 12)
  uint8_t  reserved[4];
};
```

- The directory is sorted by `(name_hash, name)`. Lookup is a binary search on
  the hash followed by a name comparison.
- The slot table of an owner maps a slot to its directory entry, so a handle
  from a change entry can be turned back into a name and type.
- A process that owns no tags (for example `scada-api`) appears only in the
  process table.
- Everything in `header` except `GlobalHeader.state` and
  `ProcessEntry.down` is immutable for the life of the generation.
- In a new generation `ProcessEntry.down` is 1 for every process but `tagd`
  and 0 for `tagd` from the moment the header is published, before `current`
  points to it: a process that has not attached has no region and is down by
  section 10.1. A reused generation keeps the flags it holds (section 11.1).

### 5.1 Handles

```
handle = (owner_index << 22) | slot        // 10 bits owner, 22 bits slot
SCADA_INVALID_HANDLE = 0xFFFFFFFF
```

A handle is valid only within the generation that produced it. After a
generation change every handle must be resolved again.

## 6. Tag records (`tags.<owner>`)

Layout: `RegionHeader`, then `tag_count` records.

```cpp
enum TagType : uint8_t { BOOL = 1, INT64 = 2, UINT64 = 3, FLOAT64 = 4, STRING = 5 };
enum Quality : uint8_t { GOOD = 0, UNCERTAIN = 1, BAD = 2 };

enum QualityFlags : uint16_t {
  STALE = 1 << 0, COMM_FAIL = 1 << 1, OUT_OF_RANGE = 1 << 2,
  SUBSTITUTED = 1 << 3,     // value entered by a person in place of the source
  NOT_INIT = 1 << 4,
  OWNER_DOWN = 1 << 5,      // added by a read (section 6.2), never stored
  TIME_UNCERTAIN = 1 << 6,
  BLOCKED = 1 << 7,         // point out of service or blocked for transmission
  TEST = 1 << 8,            // value produced in test mode
  OVERFLOW = 1 << 9,        // counter rolled over or value exceeded its format
  RESTORED = 1 << 10,       // retained value, not yet written by the owner
  LOCAL_TIME = 1 << 11      // source_time_ns was set by the owner, not a device
};

struct TagRecord {               // 64 bytes, alignas(64)
  uint32_t seq;                  // A; odd while the owner is writing
  uint8_t  type;
  uint8_t  quality;
  uint16_t quality_flags;
  uint64_t value;                // bit pattern (section 1)
  int64_t  source_time_ns;
  int64_t  update_time_ns;
  int64_t  update_mono_ns;
  uint32_t change_count;
  uint8_t  reserved[20];
};
```

Initial state of every record: `seq` 0, `quality` BAD, `quality_flags`
NOT_INIT, all times 0.

`source_time_ns` is the device timestamp when the protocol has one; otherwise
the owner sets it equal to `update_time_ns` and sets LOCAL_TIME. STALE is set
by the owner (for a driver, by the driver framework). `scada-tagd` never writes
a tag record; its only marking is the `down` flag of section 10.1.

### 6.1 Write protocol (owner thread only)

```
s = seq.load(relaxed)
seq.store(s + 1, relaxed)          // now odd
atomic_thread_fence(release)       // the data stores below must not pass the odd seq
write the fields other than seq, each with a relaxed atomic store
if value or quality or quality_flags changed: change_count += 1
seq.store(s + 2, release)          // even again
if changed: append a change entry (section 8)
```

The fence is required: on ARM64 a release store does not order the stores that
follow it. The write wakes nobody (section 8.4).

### 6.2 Read protocol (any thread of any process)

```
repeat up to 64 times:
  s1 = seq.load(acquire)
  if s1 is odd: pause instruction, continue
  copy the fields other than seq, each with a relaxed atomic load
  atomic_thread_fence(acquire)     // the copies above must complete before s2
  s2 = seq.load(relaxed)
  if s1 == s2: return the copy
return SCADA_E_BUSY                // the output is left unchanged
```

The copy by relaxed atomic loads is how the library reads memory another process is
writing without a data race; it compiles to plain moves.

`SCADA_E_BUSY` means the writer was in the middle of a write for the whole
attempt, for example a non-real-time owner preempted mid-write. It says
nothing about the value. The caller keeps its previous value for this cycle;
a dead owner is reported through the overlay below within its heartbeat
timeout.

After a successful read the library overlays the owner state: if the owner's
process is down, or the owner's region is missing or not ready, the returned
quality is BAD with OWNER_DOWN set. The stored record is not changed. "Down"
here is the reader's own judgement by the three rules of section 10.1; the
`down` flag of `scada-tagd` is not used.

What a read returns in the other cases:

| Situation | Return code | Output |
| --- | --- | --- |
| The owner's region is missing or not ready, and a tag of the owner was resolved in this context | `SCADA_OK` | Type of the tag, value 0, times 0, change count 0, quality BAD, OWNER_DOWN only |
| Every attempt found the record mid-write and the owner is down | `SCADA_OK` | The same: what a dead owner left half written is not a value |
| Every attempt found the record mid-write and the owner is not down | `SCADA_E_BUSY` | Unchanged |
| The record does not hold a value of the type of its tag: another type, a quality or flag that is none, OWNER_DOWN stored, a BOOL that is neither 0 nor 1, a STRING record that is not the number of its slot, a string length above 250 | `SCADA_E_LAYOUT` | Type of the tag, value 0, times 0, quality BAD |
| The handle is none of this generation | `SCADA_E_INVALID` | Type 0, value 0, times 0, quality BAD |
| The owner is another process and no tag of it was resolved in this context | `SCADA_E_INVALID` | Type 0, value 0, times 0, quality BAD |

Only `SCADA_E_BUSY` leaves the output unchanged. Every other error stores
quality BAD, so that a caller that keeps its output never holds on to a good
value. Where value and times are 0 they mean "no value": a consumer must not
plot, compare or act on them.

An owner cannot store OWNER_DOWN: a write with that flag is refused with
`SCADA_E_INVALID`.

A tag of another process must be resolved in the context that reads it:
resolve is where its regions are mapped (section 11.2). Resolving one tag of an
owner maps the regions of all its tags. `scada_sub_open` is not a resolve: a
ring consumer that reads the tag of a `scada_change.handle` resolves a tag of
that owner first, or the read is `SCADA_E_INVALID`. Until a tag of the owner is resolved,
a read of its tags is `SCADA_E_INVALID`, so that a forgotten resolve does not
look like a dead owner to operators and alarm logic. The library tells the two
apart by one atomic load in the read path, without a lock.

### 6.3 Owner restart

When an owner attaches to a region that already exists for the current
generation, it must repair it before setting itself running. For every record
with an odd `seq`: write quality BAD and flag NOT_INIT, then store `seq + 1`.
Values of other records are kept, so readers see the last value until the
owner refreshes it. A string slot belonging to a repaired record gets `len` 0.

The owner then increments `epoch` in its change ring header (section 8). A
crash can lose the change entry of the last write, so every consumer must
resynchronise after an owner restart; the epoch tells it to.

## 7. String values (`strings.<owner>`)

Layout: `RegionHeader`, then `string_slots` slots. A STRING tag's value is its
slot number, fixed for the generation.

```cpp
struct StringSlot {              // 256 bytes
  uint16_t len;                  // bytes used, at most 250
  uint8_t  data[250];            // UTF-8, not NUL-terminated
  uint8_t  reserved[4];
};
```

The slot has no sequence of its own; the tag record's `seq` protects it. The
owner makes the tag record's `seq` odd, writes the slot, writes the tag record
fields, then makes `seq` even. A reader reads `seq`, the tag record and the
slot, then checks `seq` as in section 6.2. A reader therefore never pairs a
string with the quality or times of another write. The reader validates `len`
before using it.

## 8. Change rings (`changes.<owner>`)

A change ring is a lossy broadcast buffer. Any number of consumers read it,
each keeping its own cursor in private memory.

```cpp
struct ChangeRingHeader {        // 64 bytes, alignas(64), follows RegionHeader
  uint32_t capacity;             // power of two
  uint32_t epoch;                // A; +1 each time the owner reattaches (section 6.3)
  uint64_t write_seq;            // A; number of entries ever written
  uint8_t  reserved[48];
};

struct ChangeEntry {             // 32 bytes
  int64_t  update_mono_ns;       // same instant as in the tag record
  uint32_t handle;
  uint8_t  type;
  uint8_t  quality;
  uint16_t quality_flags;
  uint64_t value;
  int64_t  source_time_ns;
};
```

A consumer takes `capacity` from the header region's owner table, not from
this header, and checks that the two agree at open.

A write appends an entry only when it changed value, quality or quality
flags (for a STRING tag: its text), after the record's `seq` is even again
(section 6.1). The entry holds the fields as stored, so `source_time_ns`
and LOCAL_TIME are those the record got. An entry is outside input to the
consumer: one whose handle is not a tag of the ring's owner, or whose type,
quality, flags or value the record could not hold (section 6.2), is
reported with `SCADA_E_LAYOUT` and skipped.

### 8.1 Write (owner thread only)

```
w = write_seq.load(relaxed)
atomic_thread_fence(release)       // the entry stores must not pass the previous write_seq
write entries[w & (capacity - 1)], word-wise, relaxed
write_seq.store(w + 1, release)
```

### 8.2 Read (consumer, cursor = next position wanted)

```
w1 = write_seq.load(acquire)
if cursor == w1: nothing new
if w1 - cursor >= capacity: GAP
copy entries[cursor & (capacity - 1)], word-wise, relaxed
atomic_thread_fence(acquire)
w2 = write_seq.load(relaxed)
if w2 - cursor >= capacity: GAP        // the slot may have been overwritten
validate handle, type and quality of the copy
return the copy; cursor += 1
```

### 8.3 Gap handling

A consumer remembers the ring's `epoch`. If it has changed, that is a GAP
too. So is a change of the owner's process between down and not down (the
three rules of section 10.1, which the consumer evaluates at every look): a
dead owner writes no entry, and this gap is how a consumer of the ring
learns it. An owner that dies and restarts gives two gaps to a consumer
that looked while it was down, one to a consumer that did not; the epoch
change of the restart is part of the second gap. While the owner's process
is down, an entry that still arrives (its threads may write while its main
loop is hung) is delivered with the overlay of section 6.2: quality BAD,
OWNER_DOWN added. So is a ring that appears, disappears or is replaced by
another file while a consumer is subscribed: a subscription to a ring that is missing
succeeds, and its first look at the ring once it is there is a GAP. A
consumer reads `epoch` before `write_seq` when it sets its cursor, so that
a restart between the two loads gives one GAP too many, never one too few.

A subscription starts at the end of the ring: it delivers the entries
written after it opened.

On GAP the library reports `SCADA_E_GAP` once, counts it, and sets the cursor
to the current `write_seq`. The consumer then resynchronises, in this order:
the cursor is set first, then every tag record of that owner the consumer
cares about is read. An entry delivered afterwards whose `update_mono_ns` is
older than the record read for the same tag is discarded by the consumer, so
an old value never replaces a newer one. Consumers must treat a gap as a
normal event, not an error to crash on.

A gap loses intermediate transitions for good. To keep it rare, the tag-set
builder requires `change_ring_capacity` to be at least twice the owner's tag
count, rounded up to a power of two: one change of quality on every tag of an
owner (a communication loss, a general interrogation) then fits.

A string tag's change entry carries only the slot number; the consumer reads
the current string from the slot.

### 8.4 Waiting and waking

Writing never wakes anyone: `scada_tag_write` and `scada_queue_post` make no
system call. Instead a thread calls `scada_tag_notify()`:

- It wakes the consumers of every ring and queue the calling thread wrote
  since its previous call, increments the process's `cycle_seq` (section 10)
  and wakes its waiters.
- It makes one non-blocking system call for each ring or queue written, and
  one for the cycle. It is bounded but not RT-safe. The library knows the
  rings a thread wrote because it records, for each owner, the first thread
  that writes its tags (section 13.2).
- It also looks again for missing regions of other processes (section 11.2)
  and repeats the clock check (section 13.1).
- A real-time task calls it once at the end of each cycle, after its last
  read, write and post.
- Non-real-time code calls it after each batch of writes and after each post
  (a result, an event, a request), so that a slow scan does not delay a
  command or a result.

A consumer's wait always has a timeout. The library caps it at 100 ms and
checks `write_seq` on waking, so a missed notify delays delivery but cannot
lose it.

Mechanism: a futex wait on the low 32 bits of `write_seq` (or on `cycle_seq`),
which a reader can do on a read-only shared mapping. The wait compares the
value, so a wake between the check and the wait is not lost.

- The futex is shared, not private (no `FUTEX_PRIVATE_FLAG`): the kernel
  keys it by file and offset, so a waiter on its read-only mapping and the
  owner waking through its read-write mapping meet although the addresses
  differ. The targets are little-endian, so the low 32 bits of `write_seq`
  lie at its address.
- A consumer waits with the low 32 bits of its cursor as the expected
  value; the owner wakes with `FUTEX_WAKE` for all waiters. A wait has an
  absolute `CLOCK_MONOTONIC` deadline (`FUTEX_WAIT_BITSET`).
- A consumer of several rings and queues waits on all of them at once with a
  wait set (section 13), built on `futex_waitv`, at most 127 sources per
  set.
- A waiter on `cycle_seq` is ready when the count differs from the one its
  previous wait returned with.
- A thread that must also wait on sockets or serial ports asks the wait set
  for a file descriptor. The library then runs one waiter thread for that
  wait set, which turns wake-ups into a process-local eventfd usable with
  `poll` and `epoll`. Nothing is passed between processes, so this does not
  depend on `scada-tagd`.

Verified on x86-64 and ARM64 in phase 1 (P1-07, D-32). The fallback that
was kept in reserve, one eventfd per ring passed to consumers by
`scada-tagd` over a Unix socket, is not needed.

## 9. Reliable queues (`events.*`, `cmd.*`, `result.*`)

A reliable queue has exactly one producing thread and one consuming thread,
in the processes fixed in the queue table. They are two different
processes; the tag-set builder refuses a queue with one process at both
ends. Every `cmd.<x>` queue has the answer queue `result.<x>`, whose
producer and consumer are the consumer and producer of `cmd.<x>`, so that
every command has a way back (reference 5.3 and 8.2, step 10); the tag-set
builder refuses a `cmd` queue without it. Nothing is ever overwritten.

```cpp
struct QueueHeader {             // 64 bytes, alignas(64), follows RegionHeader
  uint32_t capacity;             // power of two
  uint32_t record_size;
  uint64_t write_seq;            // A
  uint64_t base_seq;             // position of the first record of this file (9.1); never changes
  uint32_t flags;                // bit 0 BASE_SET: the creator set base_seq
  uint32_t reserved0;
  uint8_t  reserved[32];
};
```

The consumer's position is `cursors[queue_id]` in the consumer's own
`proc.<process>` region (section 10), because the consumer cannot write to the
producer's region.

### 9.1 Produce

```
if the consumer's process is down or its region is missing: return SCADA_E_PEER_DOWN
r = consumer.cursors[queue_id].load(acquire)
w = write_seq.load(relaxed)
if r > w or w - r > capacity: return SCADA_E_PEER_DOWN   // cursor is not valid
if w - r == capacity: return SCADA_E_FULL
write records[w & (capacity - 1)], with its id set from w (section 9.4)
write_seq.store(w + 1, release)
```

"Down" here uses the test of section 10.1, which the producer evaluates itself
from the consumer's heartbeat; it does not wait for `scada-tagd`.

The producer also keeps a high-water mark `H`, the position after the last
id it gave, in the cursor slot of the queue in its own `proc` region (the
slot of a queue a process produces is no cursor; section 10). It stores
`H = w + 1` (relaxed) after every refusal above and before the record, so
that a crash between that store and `write_seq` only burns the id.

Base sequence (D-49). When attach creates the queue file (11.2 step 3,
also inside a generation, when the file was removed, shrunk or damaged:
the producer removes its own unusable file first, since the rename that
publishes a region does not replace), the producer gives it a base `B`;
`write_seq` starts at `B`, `base_seq` is `B` and BASE_SET is set before
`ready`:

```
H' = own H                  if the producer's proc region was reused, else 0
R' = r + capacity           if the consumer's cursor r is readable, not 0, not all-ones
H' = 0                      if R' != 0 and H' > R'   (impossible: damage)
B  = max(H', R')            the file is created dead (BASE_SET, base and write_seq
                            all-ones) if B leaves no room for a capacity of ids
                            (below 2^48 for CMD, 9.4)
```

Every id ever given is below `r + capacity` at its post (the rule above),
and the cursor only grows, so `B` is above every id an earlier file of the
queue gave, whenever the producer's `proc` region or the consumer's cursor
survived. A cursor of 0 is left out so that the first file of a generation
starts at 0 when the consumer attached first. The range rule is unchanged:
with `r` below `B`, `w - r <= capacity` holds, and until the consumer moves
to the new file the producer may get `SCADA_E_FULL`, a refusal, never a
loss.

### 9.2 Consume

```
r = cursors[queue_id].load(relaxed)
w = write_seq.load(acquire)
if r == w: nothing new
if r > w or w - r > capacity: report, consume nothing
copy records[r & (capacity - 1)] and validate it
```

When the cursor advances depends on the queue kind:

| Kind | Cursor advances | Delivery | After a consumer crash |
| --- | --- | --- | --- |
| EVENTS, RESULT | After the record is processed (`scada_queue_ack`) | At least once | The unacknowledged record is delivered again; the consumer ignores an id it has already stored |
| CMD | Before the record is executed | At most once | The record is not delivered again |

A command is never executed twice and never executed late:

- A driver that took a request from a queue file before the file was
  created anew (9.2, D-49) still executes it within its expiry, also when
  its requester has restarted meanwhile; as before 0.15, the requester's
  restart does not withdraw a request that was delivered.

- The driver acknowledges a write request before it acts on it.
- When a driver attaches, the library drains every request still in its `cmd`
  queues without executing it and answers each with status ABORTED. Selections
  do not survive a driver restart.
- A request whose `expiry_mono_ns` has passed, or whose lifetime
  (`expiry_mono_ns - posted_mono_ns`) is longer than the limit in section 14,
  is answered EXPIRED and not executed. The library does this:
  `scada_queue_next` never returns such a request; it answers it in the
  answer queue and takes the next one. The consumer judges its copy of the
  request on its own clock, read after `write_seq`: the request must not be
  executed when its expiry is not after now, it was posted after now, it
  expires before it was posted, its lifetime is longer than the limit, or
  its expiry lies more than the limit ahead of now. The cursor moves on
  before the answer is posted, so a crash loses at most the answer. The
  answer queue of `cmd.<x>` is `result.<x>` with producer and consumer
  swapped. One thread posts to a queue, in every build; for the answer
  queue that is the thread that takes the requests of the `cmd` queue. A
  driver answers what it received, the library what it did not deliver,
  and the driver checks `expiry_mono_ns` again right before it acts on the
  device.
- If the answer cannot be posted (result queue full, requester down, or no
  answer queue in a header the tag-set builder did not make) it is dropped
  and counted; the requester sees the request
  expire. `scada_queue_next` never blocks or fails on the answer queue.

A record is outside input to the consumer. One that fails the checks of
section 9.4 is passed over with `SCADA_E_LAYOUT`; a request passed over,
when taken or by the drain at attach, is not answered, and its requester sees
it expire. A cursor and `write_seq` out of range are reported with `SCADA_E_LAYOUT` and nothing is consumed;
the producer gets `SCADA_E_PEER_DOWN` (section 9.1). The queue then stays
stopped until the next generation; each look is counted.

The consumer follows the queue's file (D-49). The `QueueHeader` of a file
is outside input: a flag bit other than BASE_SET, `reserved0` not 0, or a
base that is all-ones or beyond `write_seq` make the file one the consumer
refuses (the queue stops, below). Two mappings of the same device and inode
are the same file: a file mapped again after a look that failed is the file
in use. It uses the first file it
sees; if that file has a base and the cursor is below it, the cursor jumps
to the base (a cursor below the base always jumps: those positions are in
no file it can reach). While the name is missing, or a different file
there fails the checks of 3.1, it keeps the file it uses, whose mapping
stays valid memory. When a different file that passes the checks is under
the name, it reads the `write_seq` of the file in use again, delivers what
that file holds up to it, and only then moves to the new file, jumping to
its base and counting the positions passed over in `skipped` (an upper
bound) and the change in `replaced`. Nothing accepted into the old file is
lost while the consumer holds it, and no id repeats. The cursor never moves
back except to all-ones: `scada_queue_ack` after a jump returns
`SCADA_E_INVALID` and stores nothing. The consumer treats the queue as dead
and stores an all-ones cursor, so that the producer gets
`SCADA_E_PEER_DOWN`, when the new file has no base (made by a build before
0.15), when its base is below the `write_seq` of the file in use (ids would
repeat), when the file in use changed size in place (checked by name at
each look, and by `fstat` on a descriptor kept for each queue mapping at
each look, outside real-time code, so that a file shrunk and then replaced
is not read; `scada_queue_next` with timeout 0 makes no system call for it:
`SIGBUS`, section 3.3), and when the name reached the mapping limit
(16 files in one context). The all-ones cursor
(2^64 - 1) is reserved: no `write_seq` reaches it (a post that would make
`write_seq` equal to it is refused with `SCADA_E_FULL`), so by the range rule
every reader of a cursor finds it out of range and consumes nothing.
`scada-tagd` applies the same range rule to every queue at each liveness
check (section 10.1), for reporting only: it reads `write_seq`, then the
consumer's cursor, then `write_seq` again, and finds the queue broken when
the cursor is beyond the second `write_seq` or the first `write_seq` is more
than the capacity beyond the cursor. While both sides run, this order
cannot find a pair broken that was in range at every moment. A queue whose
region or whose consumer's `proc` region is missing, or whose consumer is
not RUNNING (its cursors are set at attach, 11.2), is not judged. It logs
a queue that became broken, and again one that no longer is, with its
name, cursor, `write_seq` and capacity, once per change. It also logs an
events or command queue that has had no usable file at two checks in a
row while its producer was not down, and again when the file is there.
It unmaps a queue file once the name refers to another file. The library
does not use the result.
On a stopped queue `scada_queue_next` waits out its timeout before it
reports it, and a wait set never reports a stopped queue as ready, so
neither spins; a consumer that calls `scada_queue_next` only after a wake
learns of the stop from `scada_queue_stats` or from a timed call.

EVENTS and RESULT: until the ack, `scada_queue_next` returns the same record
again. CMD: the cursor advances after the copy and before `scada_queue_next`
returns.

A consumer that creates a new `proc` region inside an existing generation has
lost its cursors. It sets each cursor to the queue's current `write_seq` and
reports how many records were skipped (`scada_queue_stats`, `skipped`; the
drained requests are counted in `aborted`).

### 9.3 No audit, no action

If the consumer's region is missing or its process is down, `post` returns
`SCADA_E_PEER_DOWN` and nothing is queued. This holds for event queues too.

- An action that needs an event record is refused unless the post of that
  record returned `SCADA_OK`. "Journalled" in this contract means exactly
  that: accepted into the event queue of a consumer that is not down.
- This applies to write requests of every source, AUTO included. While the
  consumer of its event queue is down, `scada-control` cannot post write
  requests, and outputs follow their configured hold or safe behaviour.
- The only bypass is emergency direct mode (reference section 11.6).
- Residual window: a consumer that has just died is seen as down only after
  its heartbeat timeout. Records accepted in that window are in shared memory
  and are delivered when the consumer restarts, unless the unit loses power
  first.
- A queue file created anew inside a generation (9.1, 9.2, D-49): posts to
  the new file are delivered after what the consumer still holds of the old
  one. Records of the old file that its consumer had not taken are lost if
  the consumer no longer holds the old file (it was not attached, or it
  restarted); the consumer counts them in `skipped`, and `scada-tagd` logs
  the recreation at WARNING with the ids that may be lost, from the
  consumer's cursor at its last check of the old file to the new base, an
  upper bound ("at most"; with no base it says the bound is unknown), as
  audit records on an events queue. Ids repeat only in the cases of 3.3.

Decided by the product owner in review P1-01 (D-23, D-24).

### 9.4 Records

```cpp
struct EventRecord {             // 256 bytes, queues of kind EVENTS
  uint64_t seq;                  // the queue's write_seq for this record, set by the library
  int64_t  time_ns;
  uint16_t kind;                 // ALARM, COMMAND, AUDIT, SYSTEM
  uint16_t payload_len;          // at most 232
  uint32_t source_handle;        // tag the event concerns, or INVALID
  uint8_t  payload[232];         // serialised message, defined with the journal
};

struct WriteRequest {            // 64 bytes, queues of kind CMD
  uint64_t request_id;           // (queue_id << 48) | write_seq, set by the library
  uint32_t target_handle;
  uint8_t  operation;            // 1 DIRECT, 2 SELECT, 3 OPERATE, 4 CANCEL, 5 READ_NOW
  uint8_t  source;               // 1 AUTO, 2 OPERATOR, 3 REMOTE
  uint8_t  value_type;           // TagType
  uint8_t  reserved0;
  uint64_t value;
  uint32_t user_id;              // the user for OPERATOR, not 0; 0 for AUTO and REMOTE
  uint32_t reserved1;
  int64_t  expiry_mono_ns;       // the driver must not execute after this
  int64_t  posted_mono_ns;
  uint64_t select_request_id;    // for OPERATE and CANCEL: the SELECT it belongs to, else 0
  uint8_t  reserved[8];
};

struct WriteResult {             // 32 bytes, queues of kind RESULT
  uint64_t request_id;
  uint8_t  status;               // 1 OK, 2 REJECTED, 3 TIMEOUT, 4 DEVICE_ERROR,
                                 // 5 NOT_AUTHORISED, 6 EXPIRED, 7 ABORTED
  uint8_t  reserved0[3];
  int32_t  protocol_error;       // driver-specific, 0 if none
  int64_t  completed_time_ns;
  uint8_t  reserved[8];
};
```

String values cannot be commanded through `WriteRequest` in layout version 1.

`WriteRequest.user_id` 0 means "no user". An OPERATOR request carries the
user whose command it is (reference 8.2, step 1, and 13.4), so it is never 0;
AUTO and REMOTE requests come from control logic and carry none. A non-zero
`user_id` is opaque to the library and to drivers; which ids exist is
defined with authentication, and how a master's user is named on a slave
with the link (D-44). The audit identity (user, and on a slave the master
node and the master's user) is in the payload of the event record that
`scada-cmd` writes before it posts the request (reference 8.2, step 8); "no
audit, no action" (section 9.3) rests on that record, not on this field. The
check on `user_id` guards against a poster's bug and is not authentication:
a producer maps its region read-write, so only the check when the request
is taken binds a poster that writes its region directly.

`EventRecord.kind`: 1 ALARM, 2 COMMAND, 3 AUDIT, 4 SYSTEM. The library sets
`EventRecord.seq`, `WriteRequest.request_id` and `WriteRequest.posted_mono_ns`
(the time of the post) at post; the poster sets the rest. That
`posted_mono_ns` is set by the library guards against a caller's bug, not
against a producer that writes its region directly; the consumer's own
clock bounds the lifetime (section 9.2).

Checks of a record, made at post (`SCADA_E_INVALID`) and when it is taken:

- Every reserved field is 0.
- Event: a kind above, `payload_len` at most 232.
- Request: `operation` and `source` of their enumerations; `value_type` BOOL,
  INT64, UINT64 or FLOAT64, or 0 with `value` 0 for CANCEL and READ_NOW; a
  BOOL value 0 or 1; `user_id` not 0 for OPERATOR and 0 for AUTO and
  REMOTE; `select_request_id` not 0 exactly for OPERATE and CANCEL; at
  post, not expired and a lifetime within the limit, by the rule of section
  9.2. The target handle is the driver's to judge.
- Result: `status` 1 to 7.
- When taken, the id of an event or request equals its position
  (`seq`, or `request_id = (queue_id << 48) | position`). A `cmd` queue
  whose `write_seq` reaches 2^48 refuses further posts with `SCADA_E_FULL`.

Record ids come from the queue's `write_seq`, which lives in the region and
survives a producer restart, so an id is never reused within a generation. Ids
start again in a new generation and after a boot; anything stored on disk
qualifies an id with the time of the record.

## 10. Process regions (`proc.<process>`)

Every process creates exactly one. It carries the heartbeat, the cycle counter
and the read cursors of every reliable queue the process consumes.

```cpp
struct ProcHeader {              // 64 bytes, alignas(64), follows RegionHeader
  uint64_t heartbeat;            // A; incremented by scada_tag_heartbeat
  int64_t  heartbeat_mono_ns;    // A
  uint32_t pid;                  // for diagnosis only; never used for a decision
  uint32_t state;                // A; 1 STARTING, 2 RUNNING, 3 STOPPING
  uint32_t cycle_seq;            // A; incremented by scada_tag_notify
  uint32_t late_beats;           // A; late heartbeats (10.1); wraps; diagnosis only
  int64_t  longest_gap_ns;       // A; longest gap between two published heartbeats
  int64_t  last_late_mono_ns;    // A; end of the latest late gap, CLOCK_MONOTONIC
  uint32_t starts;               // A; lives that reached RUNNING (10.1); wraps; diagnosis only
  uint8_t  reserved[12];
};
// followed by: uint64_t cursors[queue_count];  A; indexed by queue_id
//   for a queue the process produces, the slot holds its high-water mark (9.1)
```

A process holds an exclusive, non-blocking `flock` on its `proc` region for
its whole life. A second process with the same name fails to attach with
`SCADA_E_BUSY`, so two writers can never share a region.

`cycle_seq` lets another process run in step: a control task bound to a
synchronous scan waits for the I/O process's `cycle_seq` to change, whether or
not any tag changed in that scan.

### 10.1 Liveness

A process is **down** when any of these holds:

- its `proc` region is missing or not ready,
- its `state` is not RUNNING,
- its `heartbeat_mono_ns` is older than its `heartbeat_timeout_ms`, or
  further than `heartbeat_timeout_ms` ahead of the reader's clock.

A heartbeat time ahead of the reader's clock is normal: the heartbeat was
written after the reader read its clock. It can lie ahead only by the time
the reader spent between the two, which is far below the timeout. A time
further ahead comes from a corrupt region or a process on another clock, and
counts as down so that it cannot keep a dead process alive for readers.

The library evaluates these three rules itself on every use (the read
overlay, `scada_proc_status`, `post`), so detection does not depend on
`scada-tagd` being alive and a clean stop is seen at once.

`scada-tagd` evaluates the same three rules every `heartbeat_check_ms` and
keeps the result in the `down` flag of the process table entry. The flag is
for reporting and alarms. The library does not use it to judge a process: it
lags by up to one check interval, and a flag left by a `scada-tagd` that has
stopped would keep a restarted process down. A reader can therefore see a
process down for a moment that no check of `scada-tagd` falls into, when a
heartbeat gap is longer than the timeout and shorter than the check interval.
The process records such gaps itself, so that they can be traced:

- `scada_tag_heartbeat()` publishes `heartbeat_mono_ns` only when the new time
  is later than the published one; with several threads the published time
  never goes back and no `heartbeat` count is lost. The thread that publishes
  a time owns the gap since the time before it.
- A gap strictly longer than `heartbeat_timeout_ms` is late: readers saw the
  process down. The process then raises `last_late_mono_ns` to the time of
  that heartbeat (a maximum, relaxed) and increments `late_beats` (acq_rel).
  `longest_gap_ns` is the longest gap of any heartbeat since the region was
  created, not the gap of the latest late one. The first heartbeat after
  attach is not measured: the gap since a previous life is a restart. A reused
  region keeps its figures; a new region starts at 0.
- The figures are reported by the process itself and are a lower bound. A gap
  is measured between the clock readings of two heartbeats, not between the
  moments they were published: a heartbeat that reads its clock and is then
  preempted before it publishes is seen late by readers for longer than it
  records, and may record nothing. A process that hangs for good records
  nothing; `scada-tagd` reports it down.
- `scada-tagd` reads `late_beats` (acquire) and the gap fields at every check
  and logs `heartbeat late` at WARNING when the count grew, at most once per
  process in 10 s: growth within that time is summed into the next record,
  which is logged at the first check after it, or when `scada-tagd` stops.
  `LATE_BEATS_TOTAL` in the record lets a reader of the log see records
  that were lost. A count that is not 0 at its first look at a region is
  logged once at INFO as `heartbeat late before this check`. `process down`
  and `process up` are never held back.
- The three fields are for diagnosis only and are never used to judge
  whether a process is down. They are input to their readers.

A process that restarts into its reused region between two checks is down
for readers and never down for a check. `starts` counts it:

- Attach increments `starts` (relaxed) at step 6 of 11.2, just before the
  state is RUNNING, whose release store publishes it; a failed attach counts
  nothing. `starts` is the number of lives that reached RUNNING in the
  region. It is never reset within a region; a new region starts at 0.
  `scada-tagd` counts its own lives the same way.
- `scada-tagd` reads `starts` of every other process at every check at
  which the process is up, after loading `state` with acquire and finding
  it RUNNING, so that a RUNNING of a new life implies its count. The first
  sample of a mapping is a baseline and is not logged. When the count grew
  by n since the previous sample (modulo 2^32), it logs `process restarted`
  with n lives. Each check since the previous sample that found the
  process up after it was down accounts for one of them, a life seen as
  down and up; the others, n minus those checks and at least 0, came and
  went unseen. A check that finds the process up but no longer RUNNING when
  it reads `starts` keeps its credit for the next sample. The unseen count
  is a lower bound: a heartbeat that comes back without a restart also
  makes such a check. The record is a
  WARNING when an unseen life is in it, else a NOTICE. It is not rate
  limited: the service manager's restart limit bounds it.
- `starts` is for diagnosis only and is a lower bound: a life of a library
  before 0.14 does not count itself. It is never used to judge liveness
  and never limits restarts; the service manager's restart limit stays the
  only one.

- The heartbeat is written by `scada_tag_heartbeat()`, called from the
  process's own main loop or cycle, never from a helper thread inside the
  library: a hung main loop must stop the heartbeat.
- It is called at an interval of at most one third of the process's
  `heartbeat_timeout_ms` (defaults: 3,000 ms general, 500 ms for real-time
  processes).
- `scada-tagd` checks every `heartbeat_check_ms` (default 250 ms), sets `down`
  when a process is down by the rules above and clears it when the heartbeat
  is fresh again and `state` is RUNNING. The flag is what `scada-tagd` reports
  and alarms on. A new generation starts with the flag at 1 for every
  process but `tagd` (section 5), so a process that has attached is
  reported up at the first check that finds it up. At the first check of a
  new generation `scada-tagd` also reports, once each, the processes whose
  region is missing or not ready: they have not attached, and their flag
  does not change.
- `scada-tagd` has its own `proc.tagd` region. If its heartbeat stops, the
  library reports `SCADA_E_TAGD_DOWN` from `scada_tag_status()`, but existing
  mappings keep working.

## 11. Lifecycle

### 11.1 Start of `scada-tagd`

1. Compute the tag-set definition and its `config_hash`.
2. If `current` points to a generation with the same layout version and
   `config_hash`, reuse it: restarting `scada-tagd` does not disturb the data
   plane.
3. Otherwise remove every `gen-*` directory that `current` does not point to
   and that has no live process (left by a crash while building), create
   `gen-<n+1>/`, write `header` with state BUILDING, set `ready`, set state
   RUNNING, then switch `current`: create a symlink under a temporary name and
   rename it over `current`.

### 11.2 Attach (`scada_tag_attach`)

1. Check the base directory (section 3.1). Open `current/header`. Fail with
   `SCADA_E_LAYOUT` unless magic and layout version match the library's.
2. Find the calling process in the process table by name, and check its uid.
3. For each of the process's own regions: if it exists with the right owner,
   size and generation and is `ready`, reuse and repair it (section 6.3);
   queue positions and cursors in a reused region are kept. If it exists, is
   owned by the caller's uid, and is unready or has the wrong size, remove it.
   If it is absent, create it (section 3.2) and load retained values (section
   12) into a new `tags` region. If it exists with another owner, attach fails
   with `SCADA_E_PERMISSION` and the file is left for `scada-tagd` to remove.
4. Take the lock on `proc.<process>`. Map and pre-fault the process's own
   regions. A process with the REALTIME flag is expected to run under
   `mlockall(MCL_CURRENT | MCL_FUTURE)`.
5. Drain the process's `cmd` queues (section 9.2).
6. Set `state` RUNNING and write the first heartbeat. From here the caller
   must call `scada_tag_heartbeat()`.
7. Read `GlobalHeader.state` again. If it is RETIRING, detach and start again
   from step 1. The lock of step 4 failing in a RETIRING generation counts as
   RETIRING too, not as a second process of the name: `scada-tagd` takes such
   a lock for a moment to learn whether its process left (section 11.3).

Other processes' regions are mapped read-only and pre-faulted in
`scada_tag_resolve`, `scada_sub_open`, `scada_queue_open` and
`scada_proc_resolve`, never inside an RT-safe function. A region that is
missing or not ready is not an error: its tags read as BAD with OWNER_DOWN.
The library tries the mapping again inside `scada_tag_notify()`, at most once
a second for each missing region.

### 11.3 Generation change

1. `scada-tagd` builds the new generation as in 11.1 and sets the old header's
   state to RETIRING.
2. Each process sees RETIRING (checked on every heartbeat). A producer stops
   writing and posting and calls notify. A consumer first drains its event and
   result queues until its cursor equals `write_seq`, bounded by 1 second, so
   that no journal or audit record is dropped. Write requests still queued are
   answered ABORTED. Then the process detaches, attaches to `current`, and
   resolves its handles again. Tag writes stay allowed until the process
   detaches: what it writes there is kept by step 3. (What the library does
   to queues in a RETIRING generation is issue #65.)
3. `scada-tagd` watches every generation in the base directory other than
   `current`, also after a restart of its own, when it finds them by their
   directories (D-45):
   - An owner has left such a generation once its process stored a state
     other than RUNNING or no longer holds the lock of its `proc` region. A
     stale heartbeat alone is not enough: a hung process can resume writing.
   - When an owner has left, `scada-tagd` reads its RETAIN tags there (and
     those the current generation retains) with the read of section 6.2,
     without the owner-down overlay, and writes its retain file marked FINAL
     for that generation (section 12). It writes nothing for an owner from an
     older generation once the owner has a ready `tags` region in `current`.
   - It removes the directory once the FINAL files of all its owners are on
     disk (the directory `fsync` returned) and no `proc` region in it is
     locked; or, 30 s after it was retired (counted from the start of
     `scada-tagd` after a restart), whatever the processes do. Then the values
     of an owner still attached are written but not marked FINAL, and every
     process still holding its lock is reported with its pid.
   - Removal unlinks the files and the directory; nothing is truncated, so
     mappings that processes and observers hold stay valid. An observer holds
     no lock and does not delay removal; it learns of the change from
     `scada_tag_status` (section 11.4).
   - At its start, `scada-tagd` writes the retain files of a generation in
     which no process is attached before it removes it (section 11.1, step 3).

Generation numbers start at 1 after each boot, because `/dev/shm` is empty
then. A generation number is unique only until the next boot; anything stored
on disk identifies tags by name and tag sets by `config_hash`.

The order in which services reattach, and what outputs do meanwhile, is decided
by `scada-conf` and the output configuration, not by this contract.

### 11.4 Observe (`scada_tag_observe`)

An observer context reads the database without being a process of the
tag set. It is for tools such as `scadactl` (reference 11.5, 11.6).

1. It makes the checks of 11.2 step 1 (base directory, `current`, the
   generation directory, `header`, magic, layout version) and reads the tag
   directory, as attach does. A caller that may not open the base or
   generation directory gets `SCADA_E_PERMISSION`.
2. It needs no entry in the process table, checks no uid against it, and
   has the process index `SCADA_NO_PROCESS` (0xFFFF).
3. It creates no file, takes no lock and writes nothing to shared memory.
   Every region it reads is mapped read-only through the same checks as the
   regions of other processes (section 3.1), on demand, as in 11.2. It keeps
   no descriptor of a region.
4. Write functions return `SCADA_E_NOT_OWNER` (`SCADA_E_INVALID` for a
   handle that is none); `scada_tag_heartbeat` and `scada_tag_notify`
   return `SCADA_E_INVALID`; each before it touches shared memory.
   `scada_queue_open` returns `SCADA_E_NOT_OWNER` in an observer context,
   for any queue name and before it maps anything, so no queue can be
   posted to, acknowledged or added to a wait set from one.
5. `scada_tag_status` does for an observer what `scada_tag_notify` does
   for a process: it looks again for missing regions (at most once a second
   each) and repeats the clock check. It also detects a generation change,
   which an observer cannot see through a heartbeat: it returns
   `SCADA_E_RETIRING` when the generation is RETIRING, or when, at a look
   at most once a second: `current` names another generation (retiring is
   best effort, 11.3); the base directory under its path is another
   directory than at observe (the database was set up again); or neither
   the base directory nor `current` could be read at two looks in a row.
   An observer calls it about once a second and before it reports what it
   read.
6. On `SCADA_E_RETIRING` the observer detaches, which unmaps everything,
   observes again (step 1 retries a RETIRING generation a bounded number
   of times) and resolves its handles again.

There is no registry of observers: `scada-tagd` and the processes do not
know they are there, and any number may run.

## 12. Retained values

- A tag with the RETAIN flag is persisted by `scada-tagd`, which reads its
  record with the read of section 6.2, without the owner-down overlay, and
  writes `/var/lib/scada/retain/<owner>.bin` when values changed, at most
  once per retain interval (`--retain-interval-ms`, default 5 seconds), and
  on clean shutdown. A record counts as a value if its quality is GOOD or
  UNCERTAIN and NOT_INIT is not set. A record read consistently (an even
  `seq`, the same before and after) with quality BAD and without NOT_INIT
  removes the tag from the file at the next write: the owner wrote the tag
  and marked it bad, and a restart must not bring an older value back. A
  record with NOT_INIT (the owner has not written the tag since its region
  was created, for example because its restore failed) says nothing about
  the value and keeps it on file, as does a record that cannot be read
  consistently (mid-write at every attempt).
- A tag that also has RETAIN_NOW (modes, control authority) is written at
  the first liveness check after its change, so within 1 second; a power
  loss cannot revert it by more than that.
- A STRING tag cannot have RETAIN: the file holds 64-bit values only, and
  the tag-set builder refuses the flag.
- The directory is `scada-tagd:scada`, mode `2750`; files are `0640`.
  `scada-tagd` and the library open it with `O_NOFOLLOW` and check that it
  is a directory, its owner (the uid of `scada-tagd`), its mode and its
  group (that of the base directory); `scada-tagd` refuses to start
  otherwise and does not create it (the platform does, `tmpfiles.d`). An
  attach that finds it unusable restores nothing and counts it
  (`directory_unusable`). The library reads a file only if it is a regular
  file with one name (`st_nlink` 1; with `fs.protected_hardlinks` 1, section
  3, no other user can add a name to a file of `scada-tagd`), owned by the
  uid of `scada-tagd`, of at most 64 MiB, opened with `O_NOFOLLOW |
  O_NONBLOCK` and read, not mapped.
- The file is written atomically, by a thread of `scada-tagd` of its own,
  in exactly this order: `openat(".tmp.<owner>.bin.<pid>", O_WRONLY |
  O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC)`, `fchmod 0640`, `write` in a
  loop, `fsync`, `renameat` over `<owner>.bin`, `fsync` of the directory. On
  any error the temporary file is unlinked and a later write starts with a
  fresh one; an `fsync` is never tried again on the same descriptor. Stale
  temporary files are removed at start. An owner file `<owner>.bin` ends in
  `.bin`, a temporary file in digits, and an owner name cannot start with
  `.`: neither is taken for the other. The removal of files of owners that
  are gone, and its directory `fsync`, also run on that thread: the thread
  of the liveness checks never calls `fsync`. It waits for the writer at
  most 5 s (at the stop and in `flush`) and logs when that bound is hit;
  values still unsaved at exit are logged.
- Format, version 1, little-endian: a header of 80 bytes (magic
  "SCADARTN", format version, record count, source generation and the
  `created_time_ns` of its header, `written_time_ns`, flags with bit 0
  FINAL, a reserved field of 0, `config_hash`), then the records sorted by
  name, each `(type, name length, 6 reserved bytes, value, source_time_ns,
  name, padding to 8)`, then a CRC-32 (IEEE, as zlib) over everything before
  it. The layout is in `libs/scada-tag/src/retain_file.h`. Names, not
  handles, so it survives a generation change; `config_hash` is for
  diagnosis only and a different hash does not reject the file.
- FINAL with a generation (number and `created_time_ns`, since numbers
  start again at 1 after a boot) means: the file holds what the owner wrote
  in that generation before it left it (section 11.3). The mark stays on
  later writes of the same owner. `scada-tagd` never writes a FINAL file for
  an owner from a generation older than one in which the owner has a ready
  `tags` region, or older than the generation its file is already marked
  FINAL for: newer values are never replaced by older ones.
- The file is outside input to the library. A file that fails its checks is
  ignored, counted in `scada_tag_retain_status` and reported by
  `scada-tagd` in its log; a record whose name is unknown or whose type
  differs is skipped.
- When an owner creates a new `tags` region, the library loads the retained
  values for that owner's tags before setting `ready`. A value is taken from
  the owner's own file; for a tag the own file does not have (a tag that
  moved to this owner), from the most recently written other file that has
  it with the same type. At most 1,023 files are read.
- Before it loads them, attach waits, at most 2 seconds, looking every
  20 ms, until every owner that held one of these tags in a RETIRING
  generation still in the base directory has its file marked FINAL for the
  newest such generation, or that generation's directory is gone. When the
  wait runs out, attach takes the files as they are and counts it.
- A restored value has quality UNCERTAIN and flag RESTORED until the owner
  writes the tag, never GOOD; there is no time limit, so RESTORED lasts as
  long as the owner does not write. `source_time_ns` is the stored one, and
  `update_time_ns` and `update_mono_ns` are the time of the restore, so an
  age computed from them (`age_ns`) is the time since the restore, not the
  age of the value. A reader tells a restored value by the RESTORED flag
  alone. A tag without a stored value has its initial state (BAD,
  NOT_INIT).
- `scada-tagd` removes the file of an owner that is in no generation any
  more, once no generation is retired and every owner of `current` has its
  file.
- A retired generation that cannot be removed (something in it another
  user made and `scada-tagd` cannot unlink) is tried again at every check,
  for at most 60 s (twice the 30 s timeout) after it retired; then it is
  logged as abandoned and left until the next boot clears `/dev/shm`.

## 13. Library interface

The C interface is the stable one; the C++ interface is a thin wrapper.

```c
typedef struct {
  uint8_t  type;                 /* TagType */
  uint8_t  quality;
  uint16_t quality_flags;
  uint32_t change_count;         /* filled by read */
  uint64_t value;                /* bit pattern, section 1 */
  int64_t  source_time_ns;       /* on write: 0 means "no device time" */
  int64_t  update_time_ns;       /* filled by read; ignored by write */
  int64_t  update_mono_ns;       /* filled by read; ignored by write */
  int64_t  age_ns;               /* filled by read, from update_mono_ns */
} scada_value;                   /* 48 bytes */

typedef struct {
  const char *base_dir;          /* NULL: /dev/shm/scada */
  const char *retain_dir;        /* NULL: /var/lib/scada/retain */
  int64_t     tagd_uid;          /* -1: look up the user scada-tagd */
} scada_attach_opts;

/* Attach and status */
int  scada_tag_attach(const char *process_name, scada_ctx **out);
int  scada_tag_attach_opts(const char *process_name, const scada_attach_opts *opts, scada_ctx **out);
int  scada_tag_observe(scada_ctx **out);                                                 /* section 11.4 */
int  scada_tag_observe_opts(const scada_attach_opts *opts, scada_ctx **out);            /* retain_dir unused */
void scada_tag_detach(scada_ctx *ctx);
int  scada_tag_heartbeat(scada_ctx *ctx);              /* RT-safe */
int  scada_tag_notify(scada_ctx *ctx);                 /* wake consumers; bounded, not RT-safe */
int  scada_tag_status(scada_ctx *ctx, scada_status *out);
int  scada_tag_retain_status(scada_ctx *ctx, scada_retain_status_t *out);        /* section 12 */
int  scada_proc_resolve(scada_ctx *ctx, const char *process_name, uint16_t *index);
int  scada_proc_status(scada_ctx *ctx, uint16_t index, scada_proc_status_t *out);        /* RT-safe */
int  scada_owner_info(scada_ctx *ctx, uint16_t owner_index, scada_owner_info_t *out);
uint32_t scada_tag_handle(uint16_t owner_index, uint32_t slot);                          /* RT-safe; no context */

/* Tags */
int  scada_tag_resolve(scada_ctx *ctx, const char *name, uint32_t *handle);
int  scada_tag_info(scada_ctx *ctx, uint32_t handle, scada_tag_info_t *out);
int  scada_tag_read(scada_ctx *ctx, uint32_t handle, scada_value *out);                  /* RT-safe */
int  scada_tag_read_many(scada_ctx *ctx, const uint32_t *handles, size_t n,
                         scada_value *out, int *status);                                 /* RT-safe */
int  scada_tag_write(scada_ctx *ctx, uint32_t handle, const scada_value *v);             /* RT-safe, owner thread */
int  scada_tag_write_many(scada_ctx *ctx, const uint32_t *handles, size_t n,
                          const scada_value *v);                                         /* RT-safe, owner thread */
int  scada_tag_read_string(scada_ctx *ctx, uint32_t handle, char *buf, size_t cap,
                           size_t *len, scada_value *meta);
int  scada_tag_write_string(scada_ctx *ctx, uint32_t handle, const char *s, size_t len,
                            uint8_t quality, uint16_t flags, int64_t source_time_ns);

/* Change rings */
int  scada_sub_open(scada_ctx *ctx, const char *owner, scada_sub **out);
int  scada_sub_next(scada_sub *sub, scada_change *out, int timeout_ms);   /* SCADA_E_GAP on overrun */
int  scada_sub_stats(scada_sub *sub, scada_sub_stats_t *out);             /* gaps counted */
void scada_sub_close(scada_sub *sub);

/* Reliable queues */
int  scada_queue_open(scada_ctx *ctx, const char *queue_name, scada_queue **out);
int  scada_queue_post(scada_queue *q, const void *record, size_t size);   /* RT-safe */
int  scada_queue_next(scada_queue *q, void *record, size_t size, int timeout_ms);
int  scada_queue_ack(scada_queue *q);                  /* EVENTS and RESULT only */
int  scada_queue_stats(scada_queue *q, scada_queue_stats_t *out);   /* counts of one side */
void scada_queue_close(scada_queue *q);

/* Waiting on several sources */
int  scada_waitset_create(scada_ctx *ctx, scada_waitset **out);
int  scada_waitset_add_sub(scada_waitset *ws, scada_sub *sub);
int  scada_waitset_add_queue(scada_waitset *ws, scada_queue *q);
int  scada_waitset_add_cycle(scada_waitset *ws, uint16_t process_index);
int  scada_waitset_wait(scada_waitset *ws, int timeout_ms);
int  scada_waitset_fd(scada_waitset *ws, int *fd);     /* for poll and epoll; starts the waiter thread */
void scada_waitset_destroy(scada_waitset *ws);
```

- `scada_tag_write` uses `type`, `value`, `quality`, `quality_flags` and
  `source_time_ns` of its argument; the library sets the update times.
- `read_many` and `write_many` read the clocks once for the whole call.
  `read_many` returns a status for each handle.
- `size` in the queue calls must equal the queue's `record_size`.
- `scada_tag_read_string` with `buf` NULL and `cap` 0 returns the length and
  `meta` alone, as a large enough buffer would; `buf` NULL with `cap` above 0
  is `SCADA_E_INVALID`.
- `scada_sub_open` subscribes to one owner's ring. The per-tag filter of
  reference section 5.7 is applied by the caller on `scada_change.handle`.
- Every timeout is in ms and at least 0; 0 does not wait. There is no wait
  without a timeout.

```c
typedef struct {
  uint32_t handle;
  uint8_t  type;                 /* TagType */
  uint8_t  quality;              /* as stored; BAD while the owner is down */
  uint16_t quality_flags;        /* as stored */
  uint64_t value;                /* bit pattern; a STRING's slot number */
  int64_t  source_time_ns;
  int64_t  update_mono_ns;
} scada_change;                  /* 32 bytes */

typedef struct {
  uint64_t received;             /* changes delivered */
  uint64_t gaps;                 /* SCADA_E_GAP returned */
  uint64_t lost;                 /* entries jumped over by overruns */
  uint64_t invalid;              /* entries refused with SCADA_E_LAYOUT */
  uint32_t epoch;                /* of the ring, as last seen */
  uint16_t owner_index;
  uint16_t process_index;
  uint32_t owner_changes;        /* owner seen going down or coming back */
  uint8_t  owner_down;           /* 1 if down at the last look */
  uint8_t  last_gap;             /* why the last GAP: 0 none, 1 overrun,
                                    2 restart (epoch), 3 ring appeared,
                                    disappeared or replaced, 4 owner down,
                                    5 owner back */
  uint8_t  reserved[2];
} scada_sub_stats_t;             /* 48 bytes */
```

```c
typedef struct {                 /* one side of a queue in one context, since attach */
  uint64_t posted;               /* producer: records accepted */
  uint64_t delivered;            /* consumer: records returned, redeliveries included */
  uint64_t expired;              /* cmd consumer: requests answered EXPIRED, not returned */
  uint64_t aborted;              /* cmd consumer: requests drained at attach */
  uint64_t skipped;              /* consumer: positions passed over: at attach with a new proc region, and
                                    at a jump to a queue file's base (9.2; an upper bound); for events,
                                    possibly lost audit records */
  uint64_t answers_dropped;      /* cmd consumer: answers that could not be posted */
  uint64_t invalid;              /* consumer: records passed over as not valid */
  uint64_t out_of_range;         /* consumer: looks that found cursor and write_seq out of range */
  uint64_t replaced;             /* consumer: queue files given up inside the generation, followed (the
                                    consumer moved to the new file) or stopped (dead); a stop also counts
                                    in out_of_range at every look, a follow does not;
                                    producer: file created anew while the consumer's cursor was not 0 */
  uint16_t queue_id;
  uint16_t kind;                 /* 6 EVENTS, 7 CMD, 8 RESULT */
  uint8_t  producer;             /* 1 producer's side, 0 consumer's */
  uint8_t  reserved[3];
} scada_queue_stats_t;           /* 80 bytes */
```

#define SCADA_NO_PROCESS 0xFFFF  /* process_index of an observer (11.4) */

typedef struct {                 /* what scada_tag_status reports */
  uint64_t generation;
  uint32_t generation_state;     /* 1 BUILDING, 2 RUNNING, 3 RETIRING; 0 if not valid */
  uint16_t process_index;        /* SCADA_NO_PROCESS for an observer */
  uint8_t  tagd_down;
  uint8_t  clock_syscall;        /* section 13.1 */
  uint32_t regions_created;
  uint32_t regions_reused;
  uint32_t records_repaired;
  uint16_t owner_count;          /* owner indexes 0 .. owner_count - 1 */
  uint16_t process_count;        /* process indexes 0 .. process_count - 1 */
} scada_status;                  /* 32 bytes */

typedef struct {                 /* section 12; since attach; all 0 for an observer */
  uint32_t values_restored;      /* retained values loaded into new tags regions */
  uint32_t files_rejected;       /* retain files that failed their checks, at each look */
  uint32_t final_waits_expired;  /* waits for FINAL files that ran out */
  uint32_t directory_unusable;   /* restores not done: the retain directory failed its checks */
} scada_retain_status_t;         /* 16 bytes */

typedef struct {
  char     name[32];             /* NUL-terminated */
  uint32_t tag_count;            /* slots 0 .. tag_count - 1 */
  uint32_t change_ring_capacity;
  uint16_t process_index;
  uint8_t  owned;                /* 1 if the caller is that process */
  uint8_t  reserved0;
  uint32_t reserved1;
} scada_owner_info_t;            /* 48 bytes */

typedef struct {
  char     name[32];             /* NUL-terminated */
  uint64_t heartbeat;            /* 0 if the region is missing */
  int64_t  heartbeat_mono_ns;
  int64_t  heartbeat_age_ns;     /* never negative */
  uint32_t heartbeat_timeout_ms;
  uint32_t state;                /* ProcHeader.state as stored; 0 if missing */
  uint32_t cycle_seq;
  uint32_t pid;                  /* diagnosis only */
  uint32_t uid;                  /* from the process table */
  uint16_t process_index;
  uint8_t  down;                 /* section 10.1, judged by the library now */
  uint8_t  down_reason;          /* 0 none, 1 region missing, 2 not running,
                                    3 heartbeat stale, 4 not resolved */
  uint8_t  marked_down;          /* the `down` flag of scada-tagd */
  uint8_t  realtime;
  uint8_t  reserved[6];
} scada_proc_status_t;           /* 88 bytes */
```

`scada_tag_handle` packs a handle as section 5.1 says, so that callers do
not depend on the bit split; it returns `SCADA_INVALID_HANDLE` for an owner
index or slot beyond it. `scada_owner_info` with `scada_tag_handle` and
`scada_tag_info` lists every tag of a generation. `scada_proc_resolve`
maps the `proc` region of the process. `scada_proc_status` of a process that
was not resolved in the context (and is neither the caller nor `scada-tagd`)
returns `SCADA_E_INVALID` with down 1 and down_reason 4 (not resolved), not a
process that is down, so that a forgotten resolve is not taken for a dead
process; the fields of the process table are filled. The fields of `scada_proc_status_t` are read one by one and need
not belong to one heartbeat.

Every GAP asks for the same action, a read of the tags. The reason is for
reporting: a consumer that raises a communication alarm raises it on 4 and
clears it on 5. When the owner's state changes at the same look as its ring
appears or is replaced, the reason is the owner's.

A change carries the stored quality, with the overlay of section 6.2 while
the owner's process is down (section 8.3). A consumer that keeps copies of
values reads the tags after a GAP and takes what the read gives: BAD with
OWNER_DOWN for a dead owner, so it never holds on to a last good value.

### 13.1 Real-time safety

Functions marked RT-safe must not allocate, block, or enter the kernel. They
validate the handle with two comparisons and touch only memory that was mapped
and pre-faulted earlier.

They may read `CLOCK_REALTIME` and `CLOCK_MONOTONIC`, which both targets
normally serve from the vDSO without a system call. Some clock sources (certain
ARM timers, a time-stamp counter the kernel has demoted) make the read a real
system call. The library checks at attach, and again inside
`scada_tag_notify()` once a minute, by reading the kernel's current clock
source name and comparing the time of a clock read through the library with
the time of a raw `clock_gettime` system call. The read counts as a system
call when the source is not one the vDSO reads, or when the read takes more
than half the time of the system call (a vDSO that falls back to the system
call keeps the source name, for example with an ARM timer erratum
workaround). If the raw system call is refused, only the name decides. On
failure attach still succeeds:
`scada_tag_status()` reports it, and the process raises a system alarm,
because the real-time rule no longer holds on that unit.

### 13.2 Threads

- `scada_ctx` may be used from several threads. The read functions,
  `scada_proc_status` and `scada_tag_heartbeat` are safe from any thread.
  Heartbeats from several threads never move the published time back
  (section 10.1).
- All writes to the tags of one owner come from one thread. A `scada_sub`, a
  `scada_queue` and a `scada_waitset` are each used by one thread.
- `scada_tag_notify` acts for the calling thread: it wakes the consumers of
  what that thread wrote.
- The library does not lock to enforce these rules. A debug build checks them
  and stops the process on a violation.

### 13.3 Return codes

| Code | Value | Meaning |
| --- | --- | --- |
| `SCADA_OK` | 0 | Success |
| `SCADA_E_INVALID` | 1 | Invalid argument or handle; a value an owner may not store |
| `SCADA_E_LAYOUT` | 2 | Layout version or magic mismatch; a header or a tag record that is not valid |
| `SCADA_E_NOT_FOUND` | 3 | Unknown tag, owner, process or queue name |
| `SCADA_E_NOT_OWNER` | 4 | Write attempted by a process that does not own the tag |
| `SCADA_E_TYPE` | 5 | Value type does not match the tag type |
| `SCADA_E_FULL` | 6 | Reliable queue is full |
| `SCADA_E_PEER_DOWN` | 7 | The queue's consumer is not attached or is down |
| `SCADA_E_GAP` | 8 | Change ring overrun or owner restart; resynchronise |
| `SCADA_E_TIMEOUT` | 9 | Nothing arrived within the timeout |
| `SCADA_E_RETIRING` | 10 | The generation is retiring; reattach |
| `SCADA_E_TAGD_DOWN` | 11 | `scada-tagd` heartbeat lost |
| `SCADA_E_PERMISSION` | 12 | Uid mismatch, or a directory or region has the wrong owner or mode |
| `SCADA_E_IO` | 13 | Operating system error; `errno` is preserved |
| `SCADA_E_BUSY` | 14 | The writer was mid-write for the whole read attempt; or the process name is already attached |

## 14. Limits for layout version 1

| Item | Limit |
| --- | --- |
| Tags per generation | 1,000,000 (design target 50,000) |
| Owners | 1,023 |
| Tags per owner | 4,194,303 |
| Processes | 1,024 |
| Reliable queues | 4,096 |
| Tag name length | 127 bytes |
| String value length | 250 bytes |
| Change ring capacity | 1,024 to 1,048,576 entries (32 MB); default 65,536; at least twice the owner's tag count |
| Event payload | 232 bytes |
| Write request lifetime | 60 seconds |

The cursor array of a `proc` region has one entry for every queue of the
generation, 8 bytes each. Cursors of different queues share cache lines; at
command and event rates this is harmless.

## 15. Conformance tests

Phase 1 is accepted only when automated tests show all of these on x86-64 and
ARM64:

1. Structure sizes, alignments and offsets match this document, and no
   structure has padding.
2. 50,000 tags across 10 owners: 50,000 updates per second sustained for 10
   minutes with no torn reads observed by 4 concurrent readers.
3. A reader never observes a record with mixed old and new fields (checked
   with values whose fields are derived from one another). This also runs at
   the maximum write rate, with writers and readers on separate cores, on the
   ARM64 runner.
4. The sequence lock and the change ring are checked with a model checker for
   the ARM memory model, as written in sections 6 and 8.
5. Killing an owner with `SIGKILL` mid-write: readers return BAD with
   OWNER_DOWN within the heartbeat timeout, with `scada-tagd` running and with
   it stopped, and never hang.
6. A process that stops cleanly reads as down at once.
7. An owner restart repairs its region and readers recover without
   reattaching; every consumer of its ring gets `SCADA_E_GAP` once for the
   restart, and once more if it looked while the owner was down.
8. A non-owner process cannot write: the mapping is read-only and the library
   returns `SCADA_E_NOT_OWNER`.
9. A region file created by the wrong user is rejected and removed by
   `scada-tagd`; a symlink under a region name is not followed; a base
   directory with the wrong owner or mode is refused.
10. A process killed while creating a region attaches again on restart. A
    second process with the same name is refused.
11. Regions get the modes of section 3.1 under umask 077. Attach fails with an
    error, not a signal, when `/dev/shm` is full.
12. A library built with a different layout version refuses to attach.
13. Change ring: a slow consumer gets `SCADA_E_GAP` exactly when overrun and
    resynchronises without an old value replacing a newer one; a fast consumer
    sees every entry in order.
14. Avalanche: every tag of an owner changes quality in one cycle; no consumer
    that is keeping up gets a gap.
15. A process with several owner threads, each writing its own tags and
    posting to its own queue, passes tests 2 and 13 under the thread
    sanitizer.
16. Reliable queue, EVENTS and RESULT: no loss and no duplication in normal
    operation; `FULL` when full; redelivery after a consumer crash before
    `ack`; `PEER_DOWN` when the consumer is absent or stopped.
17. Reliable queue, CMD: a driver killed after taking a request never sees it
    again; requests queued while the driver was dead are answered ABORTED and
    not executed; an expired or over-long request is answered EXPIRED.
18. After a producer restart, record ids continue without reuse. A queue
    cursor or `write_seq` outside its valid range is rejected, not followed.
19. A post to an event queue whose consumer is down returns `PEER_DOWN`.
20. Restarting `scada-tagd` with an unchanged tag set leaves all other
    processes running without interruption.
21. A generation change completes within 5 seconds with all processes
    reattached, retained values restored, no event record lost, and no write
    request of the old generation executed.
22. RT-safe functions make no system calls and no allocations, including the
    first read of a tag after resolve (checked by instrumentation in the test
    build).
23. A string tag read never pairs a string with the quality or times of a
    different write.
24. Waiting: with notify, a consumer receives within one producer cycle; a
    process that never calls notify still delivers within the 100 ms cap; a
    wait set over 10 rings wakes for any of them; the file descriptor of a
    wait set works with `poll`; a waiter on `cycle_seq` wakes on a cycle with
    no tag change.
25. A corrupt or truncated retain file is ignored and reported; attach
    succeeds. A RETAIN_NOW tag is on disk within 1 second.
26. False trips: the reference load runs for 10 minutes with real-time readers
    sharing cores with non-real-time owners; no process is reported down and
    the count of `SCADA_E_BUSY` is reported.
27. Read latency and write latency are measured and reported: median and 99.9th
    percentile for `read`, `read_many` of 500 tags, `write`, and `write_many`
    of 500 tags.
28. Observer (section 11.4): an observer context needs no entry in the
    process table; after observe, resolve, read, subscribe, wait and status
    the generation directory is unchanged (names, inodes, sizes, times and
    bytes) and every mapping it made is read-only; every call that writes
    is refused with its code before it touches memory; several observers
    run beside a live attach of the same process; a generation change, by
    RETIRING or by `current` alone, is reported as `SCADA_E_RETIRING`; a
    caller outside the group `scada` gets `SCADA_E_PERMISSION`.
29. A queue file created anew inside a generation (sections 9.1, 9.2,
    D-49): with real processes, a producer killed at random and its file
    removed under it or after the kill, and a consumer leaving with a
    record not acknowledged, no id is delivered twice with different
    content, the cursor never moves back, every position it passed was
    delivered or counted skipped, and nothing accepted into a file the
    consumer still holds is lost; the producer's high-water mark is stored
    before the record; and the queue stops, with the all-ones cursor, for a
    new file without a base, with a base below the old `write_seq` or one
    not valid, for a file changed in size in place (also when then
    replaced), and at the 17th file under the name.

## 16. Open points to settle in phase 1

Each is settled by measurement or experiment in the issue named, recorded in
the decisions log, and then written into this contract before it becomes 1.0
(P1-13).

| Point | Proposal | Settled in | Pass criterion |
| --- | --- | --- | --- |
| Wake mechanism | Settled in P1-07 (D-32): futex on a read-only mapping, `futex_waitv` for wait sets, in-process eventfd bridge (section 8.4) | P1-07 | Conformance test 24 on x86-64 and ARM64 |
| Read retry limit before `SCADA_E_BUSY` | 64 attempts | P1-06, confirmed in P1-12 | Conformance test 26: the rate of `SCADA_E_BUSY` is below 1 in 1,000,000 reads |
| Default change ring capacity per owner | 65,536 | P1-12 | No gap for a consumer that keeps up with the reference load; database stays under 64 MB |
| Huge pages for the `tags` regions | Not used | P1-12 | Used only if the benchmark shows a lower 99.9th percentile read latency |
| Heartbeat timeout defaults | 3,000 ms general, 500 ms for real-time processes | P1-04, confirmed in P1-12 | Conformance tests 5 and 26 |
| Retained values across a generation change | Settled in P1-09 (D-45): FINAL retain files and the bounded wait of sections 11.3 and 12 | P1-09 | Conformance test 21, with a value changed immediately before the change (retained values; the queue part is #65) |
| Cost of clock reads in `write` | Two reads per `write`, one pair per `write_many` | P1-12 | A scan writing 5,000 tags spends under 0.5 ms in the library |
| Queue counts per process | `scada_queue_stats` per queue only; process-wide counts in `scada_status` are not added, since it stays 32 bytes (D-40) | P1-13 | Decided before 1.0 |
| A queue region recreated inside a generation | Settled by the base sequence (9.1, 9.2, D-49). Open in #53's second part: keeping a mapping on a transient failure in detail, the periodic look while waiting (`scada_queue_next`, wait sets) and the report of a `write_seq` that falls | #53, then P1-13 | Decided by the committee and tested before 1.0 |
| Commands after lost audit records (does not block 1.0) | Whether commands stay refused after `scada-tagd` reported lost audit records until an operator acknowledges is not specified and is safety-relevant; the library has no such latch. For the owner, before the command service (D-49, C14) | Phase 5 | Decided by the owner |
| Retiring the generation on a detected suspend | `scada-tagd` would start a generation change when it sees a suspend (3.4), so that pending requests are answered ABORTED. Restart behaviour; builds on the generation change of P1-09 | After P1-09, by the committee | Decided before 1.0 |

## 17. Differences from the reference document

This contract refines section 5 of the reference architecture. When the
contract reaches 1.0, the reference document is updated to match and the
change is recorded in its change log.

Confirmed by the product owner in review P1-01:

1. Heartbeats live in a per-process region (`proc.<process>`), not in a
   process table inside `header`, because a process cannot write to a region
   owned by `scada-tagd`. The `down` flag stays in the process table, as in
   the reference.
2. A reliable queue's read cursor lives in the consumer's `proc` region, for
   the same reason.
3. Each process creates its own regions; `scada-tagd` creates only the
   generation directories and `header`.
4. An owner is a writing thread, not a process. A process with several
   writing threads has several owners, rings and queues, so that every ring
   and queue keeps one lock-free writer.
5. Command queues deliver at most once and are drained on driver restart
   (reference 5.3 calls them reliable; reference 14.4 forbids automatic retry
   of commands).
6. Write requests of every source, AUTO included, need their event record
   accepted first (section 9.3).
7. Writers never wake consumers; `scada_tag_notify` does (section 8.4). This
   keeps the rule of reference section 5.7 that `write` and `post_request`
   make no system calls.

Smaller points where this contract adds to or narrows the reference:

8. Quality flags TIME_UNCERTAIN (reference 14.4), BLOCKED, TEST, OVERFLOW,
   RESTORED and LOCAL_TIME.
9. Write request source REMOTE (reference 8.1 and 17.4), result status
   ABORTED, and `select_request_id`.
10. A change entry carries `source_time_ns` and `update_mono_ns`, not all
    three timestamps and no sequence number of its own.
11. The tag value is a 64-bit pattern, not a union, and shared fields are
    plain integers accessed atomically, not `std::atomic` members.
12. A read that cannot complete returns `SCADA_E_BUSY`, not a BAD value.
13. Liveness is evaluated by each reader. The `down` flag `scada-tagd` keeps
    is for reporting only; readers do not use it (reference 5.4 makes it the
    source).
14. Subscription is per owner; the tag filter is applied by the caller.
15. `scada-tagd` does not mark tags stale itself; STALE is set by the owner.
16. RETAIN_NOW tags are written within 1 second; the 5-second retain interval
    is configurable.
17. The base directory is created by the platform at boot. Minimum kernel
    Linux 5.16.
18. Observer contexts (section 11.4): reference 5.7 names only attach. A
    read-only tool reads the database without an entry in the process
    table, as reference 11.6 and 18.2 (phase 1, `scadactl`) need; read
    access is membership of group `scada` (reference 4.2, 5.1).
19. Expired and over-long write requests are refused by `libscada-tag`
    (section 9.2), while reference 6.1 puts command queue handling in
    `libscada-driver`. A driver never sees an expired OPERATE or CANCEL, so
    drivers time out selections themselves.
20. The unit must not suspend or hibernate (section 3.4, D-46). The
    reference says nothing about system sleep; `scada-tagd` reports a
    suspend it detects, and does not act on it.
21. String tags cannot be retained (section 12); reference 5 does not
    restrict RETAIN to a type.
22. Reference 5.3 calls the queues reliable. A queue file created anew
    inside a generation can lose the records its consumer had not taken
    (9.3, D-49), and reference 10.4 has no alarm for lost audit records or
    a broken queue (#75; also #74, #76). Reference 18.2 puts commands
    in phase 5; the base sequence is in phase 1 so that command handling
    starts on it.

## 18. Change log

| Version | Date | Change |
| --- | --- | --- |
| 0.1 | 2026-11-13 | First draft |
| 0.2 | 2026-11-13 | Review P1-01. `down` flag moved from the owner table to the process table. Writers no longer wake consumers; `scada_tag_notify` added. String write nested inside the tag record write. Ring `epoch` added. Record ids taken from the queue `write_seq`. Checks on the base directory, region size and queue cursors. `scada_proc_status` added. Explicit padding in `GlobalHeader`. Retain file check sum. Open points given an owning issue and a pass criterion. |
| 0.3 | 2026-11-13 | Second review P1-01 (SCADA, C++ and Linux reviewers; D-24). Owners are threads; handle split 10/22 bits. Memory fences added to the sequence lock and ring. Shared fields are plain integers; value is a 64-bit pattern. `SCADA_E_BUSY` in place of a false OWNER_DOWN. Command queues at most once, drained on restart, status ABORTED, lifetime limit, `select_request_id`. AUTO writes need an event record. Liveness evaluated by readers; clean stop is down at once; heartbeat interval tied to the timeout. Wait sets, pollable descriptor, `cycle_seq`. Change entry carries `update_mono_ns`; resynchronisation order; ring capacity tied to tag count. New quality flags. Slot tables. Regions built under a temporary name; own unready files rebuilt; instance lock; `fchmod`, `openat` and `posix_fallocate` rules; base directory created by the platform; accepted risks and deployment requirements. Mapping moved out of RT-safe functions; vDSO check specified. Retain directory ownership, directory `fsync`, CRC over the header, RETAIN_NOW. Generation change drains event queues. Return codes numbered; `scada_value` and attach options defined; batch write. Conformance tests extended from 19 to 27. |

| 0.4 | 2026-11-14 | Review of P1-06 (SCADA, C++ and Linux reviewers; D-29, D-30), confirmed by the product owner. Section 10.1: a process is down by three rules; the `down` flag of `scada-tagd` is for reporting and is not used by readers. Section 6.2: what a read returns for a missing region, for a record left mid-write by a dead owner and for a record that is not valid; only `SCADA_E_BUSY` leaves the output unchanged; OWNER_DOWN is never stored; a tag is resolved in the context that reads it. Sections 6.1 and 6.2: fields are accessed one by one, not as 64-bit words. Return codes `SCADA_E_INVALID` and `SCADA_E_LAYOUT` widened. |

| 0.5 | 2026-11-14 | Change rings and the wake mechanism (P1-07, D-32). Section 8: when a write appends an entry and what it holds; entries are input; a ring that appears, disappears or is replaced is a GAP; so is the owner's process going down or coming back, and an entry of an owner that is down gets the overlay of 6.2 (D-33); epoch read before `write_seq`; a subscription starts at the end of the ring. Section 8.4: the futex mechanism verified and specified (shared futex, expected value, deadline, 127 sources per wait set, one waiter thread per wait set that asks for a descriptor, `cycle_seq` readiness); what `scada_tag_notify` does besides waking. Section 13: `scada_change` and `scada_sub_stats_t` defined; timeouts. Section 15, test 7: the second gap of a consumer that saw the owner down. Section 16: wake mechanism settled. |
| 0.6 | 2026-11-14 | Issue #41 (D-34). Section 10.1: a heartbeat time more than the timeout ahead of the reader's clock is down. Section 3.4: no private time namespace. Section 13.1: the clock check compares a library read with a raw system call. |
| 0.7 | 2026-11-14 | Smaller points of the review of P1-06 (issue #42, D-35). Section 6.2: a tag of another process none of whose tags was resolved in the context reads as `SCADA_E_INVALID`, not as OWNER_DOWN. Section 13: `scada_tag_read_string` can be asked for the length alone. |
| 0.8 | 2026-11-15 | Issue #42, point 4 (D-36). Section 10: `late_beats`, `longest_gap_ns` and `last_late_mono_ns` take 20 of the reserved bytes of `ProcHeader`. Section 10.1: the process records its late heartbeats and `scada-tagd` logs them; the published heartbeat time never goes back. Section 13.2: heartbeats from several threads. |
| 0.9 | 2026-11-15 | Issue #49 (D-42). Section 11.4: observer contexts (`scada_tag_observe`, `scada_tag_observe_opts`), what they may not do, and how they detect a generation change. Section 3.1: read access is membership of group `scada`. Section 13: `scada_owner_info`, `scada_tag_handle`, `SCADA_NO_PROCESS`, `scada_proc_status_t` defined; `scada_status.reserved1` split into `owner_count` and `process_count`. `EACCES` on the base or generation directory is `SCADA_E_PERMISSION`. `scada_proc_status` of a process not resolved in the context returns `SCADA_E_INVALID` with reason 4. Section 15, test 28. Section 17, item 18. |
| 0.10 | 2026-11-15 | Reliable queues (P1-08, D-40). Section 11.4: `scada_queue_open` of an observer. Section 9: two different processes per queue; the library answers expired requests in the answer queue, judged on the consumer's clock, one answerer per request; one posting thread in every build; records are input, checked at post and when taken, and passed over when not valid; a cursor out of range stops the queue; redelivery until the ack; the counts in `scada_queue_stats`. Section 9.4: the numbers of `EventRecord.kind`, `posted_mono_ns` set by the library and not a security boundary, the checks, the 48-bit position of a request id. Section 13: `scada_queue_stats`. Section 16: two open points. Section 17: item 19. |
| 0.11 | 2026-11-15 | Issue #58 (D-44). Section 9.4: an OPERATOR request carries a user (`user_id` not 0), checked at post and when taken; 0 means no user; a non-zero id is opaque; the audit identity is in the event record, and the check is not authentication. Section 9.2: a request passed over is not answered. |
| 0.12 | 2026-11-15 | Retained values and the removal of retired generations (P1-09, D-45). Section 11.2, step 7: a lock failure in a RETIRING generation counts as RETIRING. Section 11.3: tag writes stay allowed in a retiring generation; step 3, when an owner has left, its FINAL retain file, removal after the FINAL files or after 30 s, unlink only. Section 12: what counts as a value, the writer thread and the exact write sequence, the format, FINAL, the checks of the loader, the precedence for a moved tag, the bounded wait at attach, restored update times and the RESTORED flag, no STRING, removal of files of owners that are gone, records the owner marked BAD removed from the file, NOT_INIT records keeping the value on file, no FINAL over newer values, the checks of the retain directory, the bounded waits for the writer, the give-up on a generation that cannot be removed. Section 1: an owner name does not start with `.`. Section 13: `scada_tag_retain_status`. Section 16: retained values across a generation change settled. Section 17: item 21. |
| 0.13 | 2026-11-15 | Issue #53, points 1 and 5 (D-46). Section 9.2: `scada-tagd` applies the range rule to every queue at each check and logs a broken queue, and an events or command queue file missing for more than one check. Section 3.4: no suspend, hibernate or hybrid sleep, and how `scada-tagd` detects one. Section 3.3: the `SIGBUS` risk of its queue mappings. Section 16: retiring the generation on a detected suspend. Section 17: item 20. |
| 0.14 | 2026-11-15 | Issue #61 (D-47). Section 10: `starts` takes 4 of the reserved bytes of `ProcHeader`. Section 10.1: attach counts the lives of a process, `scada-tagd` logs restarts that no check saw; the `heartbeat late` record is limited to one per process in 10 s, with the growth summed. |
| 0.15 | 2026-11-16 | Issue #53, first part of points 2 to 4 (D-49). Section 9: `QueueHeader.base_seq` and `flags` take 16 of the reserved bytes. Section 9.1: the producer's high-water mark and the base of a file created anew. Section 9.2: the consumer follows the file, delivers the old file first, jumps to the base, and when it stops the queue. Section 9.3: the second residual window replaced. Section 10: the slot of a produced queue. Section 3.3: the residuals of the base. Sections 16 and 17. |
| 0.17 | 2026-11-16 | Issue #52 (D-52). Section 9: every `cmd.<x>` queue needs its answer queue `result.<x>` with producer and consumer swapped; the tag-set builder refuses a `cmd` queue without it. |
| 0.18 | 2026-11-15 | Issue #60 (D-53). Section 5: the start value of `ProcessEntry.down` in a new generation, as D-38 set it. Section 10.1: `scada-tagd` reports the processes not attached at the first check of a new generation. |

Migration, 0.17 to 0.18: none. The layout version stays 1, no structure in
shared memory changed, and `scada-tagd` has written these start values
since D-38; the text now states them.

Migration, 0.16 to 0.17: none for data; the layout version stays 1 and no
structure in shared memory changed. A tag set with a `cmd.<x>` queue and no
`result.<x>` queue from its consumer to its producer, which 0.16 accepted,
is now refused; add the result queue. Such a queue lost every answer before.

Migration, 0.14 to 0.15: none for data. The layout version stays 1:
`base_seq` (offset 16), `flags` (24) and `reserved0` (28) were reserved and
zero, and 32 bytes stay reserved. A queue file made by an older build has
no BASE_SET: this build reads it as before, and treats a new file without
it as the end of the queue (D-40). An older consumer reading a file made by
this build from a cursor below its base finds positions never written,
which fail their id or status check and are passed over; if it saw the old
file, it stops the queue as before. A `proc` region written by an older
producer holds 0 in the slot of a produced queue, which reads as no
high-water mark. The producer slot is no longer always 0. This build
refuses a queue file whose header has a flag bit other than BASE_SET or a
`reserved0` that is not 0; a build before 0.15 writes both as 0.

Migration, 0.13 to 0.14: none for data. The layout version stays 1: `starts`
takes 4 of the 16 reserved bytes of `ProcHeader` (offset 48), 12 stay
reserved, and the size stays 64. A region written by a library before 0.14 holds 0
there and a life of such a library leaves the count unchanged, which reads as
"no restart recorded"; a `scada-tagd` before 0.14 ignores it. The log record
`heartbeat late` gives the time as `LAST_LATE_MONO_US` (µs) in place of
`LAST_LATE_MONO_NS`; the shared-memory field `last_late_mono_ns` stays in ns.

Migration, 0.12 to 0.13: none for data; the layout version stays 1 and no
structure in shared memory changed. `scada-tagd` now maps every queue
region read-only. A unit that allowed suspend, hibernate or hybrid sleep
must disable them (`docs/install.md`).

Migration, 0.11 to 0.12: none for data; the layout version stays 1 and no
structure in shared memory changed. The retain file is new, so no file of
another format exists. A tag set with `retain` on a string tag, which 0.11
accepted and did nothing with, is now refused, as is an owner name that
starts with `.`. `scada-tagd` needs a retain directory it owns, with mode
2750 and the group of the base directory (`--retain-dir`); the installed
`tmpfiles.d` line and unit drop-in provide it. `scada_tag_retain_status`
and `scada_retain_status_t` are additions. An attach that creates a `tags`
region with RETAIN tags while a RETIRING generation lists their owner can
take up to 2 s longer.

Migration, 0.10 to 0.11: none for data; the layout version stays 1 and no
structure in shared memory changed. A poster of OPERATOR requests must set
`user_id` to the commanding user: an OPERATOR request with `user_id` 0, which
0.10 accepted, is refused at post with `SCADA_E_INVALID` and passed over
with `SCADA_E_LAYOUT` when taken.

Migration, 0.9 to 0.10: none for data; the layout version stays 1 and no
structure in shared memory changed. The queue functions are new; nothing
used them before. A tag set with a queue whose producer and consumer are
one process is now refused.

Migration, 0.8 to 0.9: none for data; the layout version stays 1 and no
structure in shared memory changed. `scada_status` keeps its size and the
offsets of every other field; the former `reserved1`, which was always 0,
now holds the owner and process counts, so a caller that checked it for 0
must stop doing so. A caller that may not open the base directory got
`SCADA_E_IO` with `EACCES` and now gets `SCADA_E_PERMISSION`. The new
functions and structures are additions.

Migration, 0.7 to 0.8: none for data. The layout version stays 1: the new
fields were reserved and zero, and 0 reads as "none recorded". A region
written by a 0.7 library holds zeros there, and a 0.7 `scada-tagd` ignores
them. Nothing judges liveness by them.

Migration, 0.6 to 0.7: none for data. The layout version stays 1 and no
structure in shared memory changed. A caller that read a tag of another
process without resolving it in its context got BAD with OWNER_DOWN and now
gets `SCADA_E_INVALID` with quality BAD; it must resolve the tag first, as 0.4
already required. A caller that passed `buf` NULL with `cap` 0 to
`scada_tag_read_string` got `SCADA_E_INVALID` and now gets `SCADA_OK` with the
length.

Migration, 0.5 to 0.6: none for data; the layout version stays 1 and no
structure changed. A process whose heartbeat time lies more than its timeout
ahead of a reader's clock, which 0.5 counted as alive, is now down. A unit
that runs a process in a private time namespace must drop it.

Migration, 0.4 to 0.5: none for data. The layout version stays 1 and no
structure in shared memory changed. `scada_change` and `scada_sub_stats_t`
are new; nothing used them before.

Migration, 0.3 to 0.4: none for data. The layout version stays 1 and no
structure changed, so regions written under 0.3 are read unchanged. Only
`libscada-tag` implements the contract, and it is at 0.4. A caller written
against 0.3 must not rely on the output of a failed read being unchanged,
except for `SCADA_E_BUSY`.

No migration is needed: nothing implements 0.1 or 0.2, and the layout version
stays 1.
