# Contract: tag database

| | |
| --- | --- |
| Contract version | 0.2 (draft) |
| Layout version | 1 |
| Status | Accepted for phase 1 implementation (review P1-01, section 18). Becomes 1.0 when phase 1 passes its acceptance tests. |
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
  `static_assert` in the library.
- "Atomic" fields are accessed with `std::atomic` operations and must be
  lock-free on both targets.
- Times are nanoseconds as `int64_t`. `*_time_ns` is `CLOCK_REALTIME` (UTC).
  `*_mono_ns` is `CLOCK_MONOTONIC`. Ages and timeouts always use monotonic time.
- Names are ASCII, lower case, `[a-z0-9_.-]`, at most 127 bytes for tags and
  31 bytes for owners, processes and queues.

## 2. Terms

| Term | Meaning |
| --- | --- |
| Owner | The one process allowed to write a given tag |
| Region | One file in the database directory, mapped into memory |
| Generation | One complete set of regions built from one configuration |
| Handle | A 32-bit reference to a tag, valid for one generation |
| Change ring | Lossy broadcast buffer of tag changes, one per owner |
| Reliable queue | Lossless single-producer, single-consumer queue |

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
    cmd.<driver>.<source>              written by the source (control or cmd)
    result.<driver>.<source>           written by the driver
```

### 3.1 Permissions

- `/dev/shm/scada/` is owned by `scada-tagd:scada`, mode `2750`: only
  `scada-tagd` can create a generation or switch `current`. If the directory
  already exists with another owner or mode, `scada-tagd` refuses to start.
- `gen-<generation>/` is owned by `scada-tagd:scada`, mode `3770` (setgid and
  sticky): group members can create files, and only a file's owner can remove
  it.
- Every region file has mode `0640` and group `scada`. Its owner is the user of
  the process that writes it.
- A process maps its own regions read-write and all others read-only.
- Before trusting a region, a reader must check that the file's owner uid
  equals the uid recorded for that owner or process in the header, that the
  file size equals the size the header gives, and that the `RegionHeader`
  fields (magic, layout version, kind, generation, size, index) match. A
  mismatch is treated as "region missing" and reported.
- The `header` file must be owned by the same uid as `/dev/shm/scada/`.
- A process resolves `current` once at attach and opens every region relative
  to that directory, so a switch of `current` cannot mix two generations.

### 3.2 Who creates what

`scada-tagd` creates the directory and the `header` region, which contains the
size of every other region. Each process creates its own regions from those
sizes when it attaches. This keeps `scada-tagd` unprivileged: it never needs
to create files on behalf of another user.

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
  std::atomic<uint32_t> ready;   // 0 while being initialised, 1 when usable
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
queue table, tag directory, name pool. Offsets are from the start of the file.

```cpp
struct GlobalHeader {            // 128 bytes
  std::atomic<uint32_t> state;   // 1 BUILDING, 2 RUNNING, 3 RETIRING
  uint32_t tag_count;
  uint16_t owner_count;
  uint16_t process_count;
  uint16_t queue_count;
  uint16_t reserved0;
  uint32_t heartbeat_check_ms;   // how often tagd checks heartbeats
  uint32_t reserved1;            // explicit padding
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
  uint16_t process_index;        // the process that is this owner
  uint16_t reserved0;
  uint8_t  reserved[12];
};

struct ProcessEntry {            // 64 bytes
  char     name[32];
  uint32_t uid;
  uint32_t heartbeat_timeout_ms;
  uint32_t flags;                // bit 0: REALTIME
  std::atomic<uint32_t> down;    // written by scada-tagd only (section 10.1)
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
  uint8_t  flags;                // bit 0 RETAIN, bit 1 WRITABLE_BY_COMMAND
  uint8_t  reserved[4];
};
```

- The directory is sorted by `(name_hash, name)`. Lookup is a binary search on
  the hash followed by a name comparison.
- An owner is always exactly one process. A process that owns no tags (for
  example `scada-api`) appears only in the process table.
- Everything in `header` except `GlobalHeader.state` and
  `ProcessEntry.down` is immutable for the life of the generation.
- No structure has implicit padding; every gap is a named reserved field,
  written as zero.

### 5.1 Handles

```
handle = (owner_index << 24) | slot        // 8 bits owner, 24 bits slot
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
  SUBSTITUTED = 1 << 3, NOT_INIT = 1 << 4, OWNER_DOWN = 1 << 5,
  TIME_UNCERTAIN = 1 << 6
};

struct TagRecord {               // 64 bytes, alignas(64)
  std::atomic<uint32_t> seq;     // odd while the owner is writing
  uint8_t  type;
  uint8_t  quality;
  uint16_t quality_flags;
  union { bool b; int64_t i; uint64_t u; double f; uint64_t str_slot; } value;
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
the owner sets it equal to `update_time_ns`. STALE is set by the owner (for a
driver, by the driver framework). `scada-tagd` never writes a tag record; its
only marking is the `down` flag of section 10.1.

### 6.1 Write protocol (owner only)

```
s = seq.load(relaxed)
seq.store(s + 1, release)          // now odd
write type-independent fields and value
if value or quality or quality_flags changed: change_count += 1
seq.store(s + 2, release)          // even again
if changed: append a change entry (section 8)
```

The write wakes nobody (section 8.4).

### 6.2 Read protocol (any process)

```
repeat up to 64 times:
  s1 = seq.load(acquire)
  if s1 is odd: spin briefly, continue
  copy the record
  s2 = seq.load(acquire)
  if s1 == s2: return the copy
return BAD | OWNER_DOWN            // writer stalled or died mid-write
```

After a successful read the library overlays the owner state: if `down` of
the owner's process is non-zero, or the owner's region is missing or not ready, the
returned quality is BAD with OWNER_DOWN set. The stored record is not changed.

### 6.3 Owner restart

When an owner attaches to a region that already exists for the current
generation, it must repair it before setting itself running: every record with
an odd `seq` gets `seq + 1`, quality BAD and flag NOT_INIT. Values of other
records are kept, so readers see the last value until the owner refreshes it.
A string slot with an odd `seq` gets `seq + 1` and `len` 0.

The owner then increments `epoch` in its change ring header (section 8). A
crash can lose the change entry of the last write, so every consumer must
resynchronise after an owner restart; the epoch tells it to.

## 7. String values (`strings.<owner>`)

Layout: `RegionHeader`, then `string_slots` slots. A STRING tag's
`value.str_slot` is fixed for the generation.

```cpp
struct StringSlot {              // 256 bytes
  std::atomic<uint32_t> seq;     // same protocol as TagRecord
  uint16_t len;                  // bytes used, at most 250
  uint8_t  data[250];            // UTF-8, not NUL-terminated
};
```

The slot is written inside the tag record's write: the owner makes the tag
record's `seq` odd, writes the slot under the slot's own `seq`, writes the tag
record fields, then makes the tag record's `seq` even. A reader reads the tag
record, then the slot, then checks the tag record's `seq` again and retries if
it changed. This way a reader never pairs a new string with old quality or
times.

## 8. Change rings (`changes.<owner>`)

A change ring is a lossy broadcast buffer. Any number of consumers read it,
each keeping its own cursor in private memory.

```cpp
struct ChangeRingHeader {        // 64 bytes, follows RegionHeader
  uint32_t capacity;             // power of two
  std::atomic<uint32_t> epoch;   // +1 each time the owner reattaches (section 6.3)
  std::atomic<uint64_t> write_seq;   // number of entries ever written
  uint8_t  reserved[48];
};

struct ChangeEntry {             // 32 bytes
  uint64_t seq;                  // equals its position in the stream
  uint32_t handle;
  uint8_t  type;
  uint8_t  quality;
  uint16_t quality_flags;
  union { bool b; int64_t i; uint64_t u; double f; uint64_t str_slot; } value;
  int64_t  source_time_ns;
};
```

### 8.1 Write (owner only)

```
w = write_seq.load(relaxed)
entries[w & (capacity - 1)] = entry with seq = w
write_seq.store(w + 1, release)
```

### 8.2 Read (consumer, cursor = next seq wanted)

```
w1 = write_seq.load(acquire)
if cursor == w1: nothing new
if w1 - cursor > capacity: GAP
copy entries[cursor & (capacity - 1)]
w2 = write_seq.load(acquire)
if w2 - cursor >= capacity: GAP        // the slot may have been overwritten
return the copy; cursor += 1
```

### 8.3 Gap handling

A consumer remembers the ring's `epoch`. If it has changed, that is a GAP too.

On GAP the library reports `SCADA_E_GAP` once, sets the cursor to the current
`write_seq`, and the consumer must resynchronise by reading every tag record
of that owner it cares about. Consumers must treat a gap as a normal event,
not an error to crash on.

A string tag's change entry carries only the slot number; the consumer reads
the current string from the slot.

### 8.4 Waiting

Writing never wakes anyone: `scada_tag_write` and `scada_queue_post` make no
system call. Instead:

- A process calls `scada_tag_notify()` once at the end of each cycle or scan,
  outside its real-time section. It wakes the consumers of every ring and
  queue the process wrote since the previous call. It is not RT-safe.
- A consumer's wait always has a timeout. The library caps it at 100 ms and
  checks `write_seq` on waking, so a missed notify delays delivery but cannot
  lose it.

Proposed mechanism for the wake: a futex wait on the low 32 bits of
`write_seq`, which a reader can do on a read-only shared mapping. This must be
verified on both targets in phase 1. If it does not hold, the fallback is one
eventfd per ring, passed to consumers by `scada-tagd` over a Unix socket. The
choice is recorded in the decisions log and then fixed here (section 16).

## 9. Reliable queues (`events.*`, `cmd.*`, `result.*`)

A reliable queue has exactly one producer and one consumer, both fixed in the
queue table. Nothing is ever overwritten.

```cpp
struct QueueHeader {             // 64 bytes, follows RegionHeader
  uint32_t capacity;             // power of two
  uint32_t record_size;
  std::atomic<uint64_t> write_seq;
  uint8_t  reserved[48];
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
records[w & (capacity - 1)] = record, with its id set from w (section 9.3)
write_seq.store(w + 1, release)
```

The consumer validates in the same way: a `write_seq` behind its cursor, or
more than `capacity` ahead, is reported and nothing is consumed.

### 9.2 Consume

```
r = cursors[queue_id].load(relaxed)
if r == write_seq.load(acquire): nothing new
process records[r & (capacity - 1)]
cursors[queue_id].store(r + 1, release)
```

Delivery is at least once: if the consumer crashes after processing a record
but before advancing its cursor, the record is delivered again on restart.
Consumers must therefore ignore a record id they have already handled.

If the consumer's region is missing or its process is down (`down` in the
process table, section 10.1), `post` returns `SCADA_E_PEER_DOWN` and nothing
is queued. This holds for event queues too: while the consumer of an event
queue is down, an action that needs that event is refused ("no audit, no
action"). Confirmed in review P1-01.

### 9.3 Records

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
  union { bool b; int64_t i; uint64_t u; double f; } value;
  uint32_t user_id;              // 0 for AUTO and REMOTE
  uint32_t reserved1;
  int64_t  expiry_mono_ns;       // the driver must not execute after this
  int64_t  posted_mono_ns;
  uint8_t  reserved[16];
};

struct WriteResult {             // 32 bytes, queues of kind RESULT
  uint64_t request_id;
  uint8_t  status;               // 1 OK, 2 REJECTED, 3 TIMEOUT, 4 DEVICE_ERROR,
                                 // 5 NOT_AUTHORISED, 6 EXPIRED
  uint8_t  reserved0[3];
  int32_t  protocol_error;       // driver-specific, 0 if none
  int64_t  completed_time_ns;
  uint8_t  reserved[8];
};
```

String values cannot be commanded through `WriteRequest` in layout version 1.

Record ids come from the queue's `write_seq`, which lives in the region and
survives a producer restart. A restarted producer therefore never reuses an
id within a generation, and a consumer that ignores ids it has already handled
cannot drop a new command by mistake.

## 10. Process regions (`proc.<process>`)

Every process creates exactly one. It carries the heartbeat and the read
cursors of every reliable queue the process consumes.

```cpp
struct ProcHeader {              // 64 bytes, follows RegionHeader
  std::atomic<uint64_t> heartbeat;      // incremented at least once a second
  std::atomic<int64_t>  heartbeat_mono_ns;
  uint32_t pid;
  std::atomic<uint32_t> state;          // 1 STARTING, 2 RUNNING, 3 STOPPING
  uint8_t  reserved[40];
};
// followed by: std::atomic<uint64_t> cursors[queue_count];  indexed by queue_id
```

### 10.1 Liveness

- `scada-tagd` checks every `heartbeat_check_ms` (default 250 ms).
- If a process's `heartbeat_mono_ns` is older than its `heartbeat_timeout_ms`
  (default 3,000 ms), or its region is missing, `scada-tagd` sets `down = 1`
  in that process's entry in the process table. Every process has the flag,
  whether or not it owns tags.
- `down` is cleared when the heartbeat is fresh again and `state` is RUNNING.
- The heartbeat is written by `scada_tag_heartbeat()`, called from the
  process's own main loop or cycle, never from a helper thread inside the
  library: a hung main loop must stop the heartbeat.
- Any process can ask whether another is down with `scada_proc_status()`
  (for example a driver deciding that `scada-control` has stopped).
- `scada-tagd` has its own `proc.tagd` region. If its heartbeat stops, the
  library reports `SCADA_E_TAGD_DOWN` from `scada_tag_status()`, but existing
  mappings keep working.

## 11. Lifecycle

### 11.1 Start of `scada-tagd`

1. Compute the tag-set definition and its `config_hash`.
2. If `current` points to a generation with the same layout version and
   `config_hash`, reuse it: restarting `scada-tagd` does not disturb the data
   plane.
3. Otherwise create `gen-<n+1>/`, write `header` with state BUILDING, set
   `ready`, set state RUNNING, then switch the `current` symlink by rename.

### 11.2 Attach (`scada_tag_attach`)

1. Open `current/header`. Fail with `SCADA_E_LAYOUT` unless magic and layout
   version match the library's.
2. Find the calling process in the process table by name, and check its uid.
3. Create the process's own regions with `O_CREAT | O_EXCL` at the sizes the
   header gives. If a region already exists with the right owner, size and
   generation, reuse and repair it (section 6.3); queue positions and
   cursors in a reused region are kept. If it exists but does not match,
   attach fails with `SCADA_E_PERMISSION` and the file is left untouched.
   Load retained values (section 12) into a newly created `tags` region. Set
   `ready`.
4. Set `state` RUNNING and write the first heartbeat. From here the caller
   must call `scada_tag_heartbeat()`.
5. Map other regions read-only on first use. A region that is missing or not
   ready is not an error: its tags read as BAD with OWNER_DOWN, and the library
   retries the mapping in the background.

### 11.3 Generation change

1. `scada-tagd` builds the new generation as in 11.1 and sets the old header's
   state to RETIRING.
2. Each process sees RETIRING (checked on every heartbeat), finishes its current
   cycle, detaches, attaches to `current`, and resolves its handles again.
3. `scada-tagd` removes the old directory when no process region in it has a
   fresh heartbeat, or after a timeout (default 30 s).

Generation numbers start at 1 after each boot, because `/dev/shm` is empty
then. A generation number is unique only until the next boot; anything stored
on disk identifies tags by name and tag sets by `config_hash`.

The order in which services reattach, and what outputs do meanwhile, is decided
by `scada-conf` and the output configuration, not by this contract.

## 12. Retained values

- A tag with the RETAIN flag is persisted by `scada-tagd`, which reads it like
  any other consumer and writes `/var/lib/scada/retain/<owner>.bin` at most
  every 5 seconds when values changed, and on clean shutdown.
- The file is written atomically: temporary file, `fsync`, rename. It holds a
  header (magic, format version, `config_hash`, record count, CRC-32 of the
  records) and `(tag name, type, value, source_time_ns)` records. Names, not
  handles, so it survives a generation change; `config_hash` is for diagnosis
  only and a different hash does not reject the file.
- The file is outside input to the library. A file that fails its checks is
  ignored and reported; a record whose name is unknown or whose type differs
  is skipped.
- When an owner creates a new `tags` region, the library loads the retained
  values for that owner's tags before setting `ready`. A restored value has
  quality UNCERTAIN and flag SUBSTITUTED until the owner writes it.

## 13. Library interface

The C interface is the stable one; the C++ interface is a thin wrapper.

```c
/* Attach and status */
int  scada_tag_attach(const char *process_name, scada_ctx **out);
void scada_tag_detach(scada_ctx *ctx);
int  scada_tag_heartbeat(scada_ctx *ctx);              /* RT-safe; call at least once a second */
int  scada_tag_notify(scada_ctx *ctx);                 /* wake consumers; once per cycle, not RT-safe */
int  scada_tag_status(scada_ctx *ctx, scada_status *out);
int  scada_proc_status(scada_ctx *ctx, const char *process_name, scada_proc_status_t *out);  /* RT-safe after first call */

/* Tags */
int  scada_tag_resolve(scada_ctx *ctx, const char *name, uint32_t *handle);
int  scada_tag_info(scada_ctx *ctx, uint32_t handle, scada_tag_info_t *out);
int  scada_tag_read(scada_ctx *ctx, uint32_t handle, scada_value *out);          /* RT-safe */
int  scada_tag_read_many(scada_ctx *ctx, const uint32_t *handles, size_t n,
                         scada_value *out);                                      /* RT-safe */
int  scada_tag_write(scada_ctx *ctx, uint32_t handle, const scada_value *v);     /* RT-safe, owner only */
int  scada_tag_read_string(scada_ctx *ctx, uint32_t handle, char *buf, size_t cap, size_t *len);
int  scada_tag_write_string(scada_ctx *ctx, uint32_t handle, const char *s, size_t len,
                            uint8_t quality, uint16_t flags, int64_t source_time_ns);

/* Change rings */
int  scada_sub_open(scada_ctx *ctx, const char *owner, scada_sub **out);
int  scada_sub_next(scada_sub *sub, scada_change *out, int timeout_ms);   /* SCADA_E_GAP on overrun */
void scada_sub_close(scada_sub *sub);

/* Reliable queues */
int  scada_queue_open(scada_ctx *ctx, const char *queue_name, scada_queue **out);
int  scada_queue_post(scada_queue *q, const void *record);                /* RT-safe */
int  scada_queue_next(scada_queue *q, void *record, int timeout_ms);
int  scada_queue_ack(scada_queue *q);                                     /* advance the cursor */
void scada_queue_close(scada_queue *q);
```

`scada_value` carries type, value, quality, quality flags, the three
timestamps, and `age_ns` computed from `update_mono_ns` at the time of the
read.

Functions marked RT-safe must not allocate, block, or enter the kernel. They
may read `CLOCK_REALTIME` and `CLOCK_MONOTONIC`, which both targets serve from
the vDSO without a system call; the library checks at attach that this holds.
Waking consumers is done only by `scada_tag_notify` (section 8.4).

`scada_sub_open` subscribes to one owner's ring. The per-tag filter of
reference section 5.7 is applied by the caller on `scada_change.handle`.

### 13.1 Return codes

| Code | Meaning |
| --- | --- |
| `SCADA_OK` (0) | Success |
| `SCADA_E_LAYOUT` | Layout version or magic mismatch |
| `SCADA_E_NOT_FOUND` | Unknown tag, owner, process or queue name |
| `SCADA_E_NOT_OWNER` | Write attempted by a process that does not own the tag |
| `SCADA_E_TYPE` | Value type does not match the tag type |
| `SCADA_E_FULL` | Reliable queue is full |
| `SCADA_E_PEER_DOWN` | The queue's consumer is not attached or is down |
| `SCADA_E_GAP` | Change ring overrun; resynchronise |
| `SCADA_E_TIMEOUT` | Nothing arrived within the timeout |
| `SCADA_E_RETIRING` | The generation is retiring; reattach |
| `SCADA_E_TAGD_DOWN` | `scada-tagd` heartbeat lost |
| `SCADA_E_PERMISSION` | Uid mismatch, or a region has the wrong owner |
| `SCADA_E_IO` | Operating system error; `errno` is preserved |

## 14. Limits for layout version 1

| Item | Limit |
| --- | --- |
| Tags per generation | 1,000,000 (design target 50,000) |
| Owners | 255 |
| Tags per owner | 16,777,215 |
| Processes | 1,024 |
| Reliable queues | 4,096 |
| Tag name length | 127 bytes |
| String value length | 250 bytes |
| Change ring capacity | 1,024 to 16,777,216 entries, default 65,536 |
| Event payload | 232 bytes |

## 15. Conformance tests

Phase 1 is accepted only when automated tests show all of these on x86-64 and
ARM64:

1. Structure sizes, alignments and offsets match this document.
2. 50,000 tags across 10 owners: 50,000 updates per second sustained for 10
   minutes with no torn reads observed by 4 concurrent readers.
3. A reader never observes a record with mixed old and new fields (checked
   with values whose fields are derived from one another).
4. Killing an owner with `SIGKILL` mid-write: readers return BAD with
   OWNER_DOWN within the heartbeat timeout plus one check interval, and never
   hang.
5. An owner restart repairs its region and readers recover without
   reattaching.
6. A non-owner process cannot write: the mapping is read-only and the library
   returns `SCADA_E_NOT_OWNER`.
7. A region file created by the wrong user is rejected.
8. A library built with a different layout version refuses to attach.
9. Change ring: a slow consumer gets `SCADA_E_GAP` exactly when overrun and
   resynchronises correctly; a fast consumer sees every entry in order.
10. Reliable queue: no loss and no duplication in normal operation; `FULL` when
    full; redelivery after a consumer crash before `ack`; `PEER_DOWN` when the
    consumer is absent.
11. Restarting `scada-tagd` with an unchanged tag set leaves all other
    processes running without interruption.
12. A generation change completes with all processes reattached, and retained
    values are restored.
13. RT-safe functions make no system calls and no allocations (checked by
    instrumentation in the test build).
14. A string tag read never pairs a string with the quality or times of a
    different write.
15. After a producer restart, record ids continue without reuse; after an
    owner restart, every consumer of its ring gets `SCADA_E_GAP` once.
16. A queue cursor or `write_seq` outside its valid range is rejected, not
    followed.
17. A process that never calls `scada_tag_notify` still delivers: consumers
    receive within the wait cap. With notify, they receive within one cycle.
18. A corrupt or truncated retain file is ignored and reported; attach
    succeeds.
19. Read latency and write latency are measured and reported: median and 99.9th
    percentile for `read`, `read_many` of 500 tags, and `write`.

## 16. Open points to settle in phase 1

Each is settled by measurement or experiment in the issue named, recorded in
the decisions log, and then written into this contract before it becomes 1.0
(P1-13).

| Point | Proposal | Settled in | Pass criterion |
| --- | --- | --- | --- |
| Wake mechanism behind `scada_tag_notify` | Futex on a read-only mapping; eventfd as fallback (section 8.4) | P1-07 | A consumer with a read-only mapping is woken on x86-64 and ARM64 |
| Read retry limit before reporting OWNER_DOWN | 64 attempts | P1-06, confirmed in P1-12 | No false OWNER_DOWN in the 10-minute reference load run, with non-real-time owners |
| Default change ring capacity per owner | 65,536 | P1-12 | No gap for a consumer that keeps up with the reference load; database stays under 64 MB |
| Huge pages for the `tags` regions | Not used | P1-12 | Used only if the benchmark shows a lower 99.9th percentile read latency |
| Heartbeat timeout defaults | 3,000 ms general, 500 ms for real-time processes | P1-04, confirmed in P1-12 | No false `down` under the reference load; conformance test 4 passes |
| Retained values across a generation change | `scada-tagd` writes each owner's retain file after that owner leaves RUNNING in the old generation; attach to the new generation waits for it, with a bound | P1-09 | Conformance test 12, with a value changed immediately before the change |

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
   directories and `header`.

Smaller points where this contract adds to or narrows the reference:

4. Quality flag TIME_UNCERTAIN (reference section 14.4 asks for the flag;
   section 5.2 does not list it).
5. Write request source REMOTE (reference section 8.1 and 17.4; section 5.3
   lists only AUTO and OPERATOR).
6. A change entry carries `source_time_ns` only, not all three timestamps.
7. Writers never wake consumers; `scada_tag_notify` does, once per cycle
   (section 8.4). This keeps the rule of reference section 5.7 that `write`
   and `post_request` make no system calls.
8. Subscription is per owner; the tag filter is applied by the caller.
9. `scada-tagd` does not mark tags stale itself; STALE is set by the owner.

## 18. Change log

| Version | Date | Change |
| --- | --- | --- |
| 0.1 | 2026-11-13 | First draft |
| 0.2 | 2026-11-13 | Review P1-01. `down` flag moved from the owner table to the process table. Writers no longer wake consumers; `scada_tag_notify` added. String write nested inside the tag record write. Ring `epoch` added. Record ids taken from the queue `write_seq`. Checks on the base directory, region size and queue cursors. `scada_proc_status` added. Explicit padding in `GlobalHeader`. Retain file check sum. Open points given an owning issue and a pass criterion. |

No migration is needed: nothing implements 0.1, and the layout version stays 1.
