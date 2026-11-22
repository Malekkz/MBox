# Contract: driver interface

| | |
| --- | --- |
| Contract version | 0.1 (draft) |
| Status | Draft for review by a person (issue #99, P2-02). No code for P2-07 to P2-10 (#104 to #107) starts before that review |
| Implements | Reference architecture 1.1, sections 6, 8.1 and 14.4; tag database contract 1.0, sections 6.3, 9 and 9.5 |
| Owners of this contract | `libscada-driver`; every `scada-drv-<protocol>` and `scada-io` |
| Meets | Configuration schemas contract (#98, `config-schemas.md`), at the points of section 15 |

This contract fixes the boundary between `libscada-driver` (the framework)
and a protocol implementation, exactly enough that a protocol can be written
and tested against the framework, and the framework against a test protocol,
each on its own.

Version 0.x may change freely within phase 2 (contracts `README.md`). The
places marked **open: needs the product owner's ruling (X-n)** carry a
conflict with the reference document or a choice the reference leaves to a
person; the draft is written with the option proposed in each, and section
16 lists them all. The places marked **review by a person** touch command
handling, select-before-operate, fail-safe or restart behaviour, or secrets
(`CLAUDE.md`, "Safety and security rules").

Input from the field: the device survey of 2026-11-22 (not in the
repository). It reviewed manuals of protection relays, inverters, plant
loggers and controllers, meters and serial sensors over several protocols.
Its findings are described here generically; every example in this contract
is invented.

## 1. Conventions

- The words **must**, **should** and **may** are used as in the reference
  document (section 1).
- **Phase 2** marks what `libscada-driver` (#104), the Modbus driver (#105,
  #106) and `scada-io` (#107) must implement. **Room** marks a part whose
  shape this contract fixes now and which a later phase implements; a
  phase 2 protocol inherits a default for it and need not know it exists.
  Section 14 lists every part with its phase.
- C++ below shows the interface of `libscada-driver`. Names follow
  `CLAUDE.md`. Every fallible call returns `scada::Status` or
  `scada::Result<T>`; no exception crosses the interface, in either
  direction. A protocol that wraps a throwing library catches at its own
  call site (coding standard, section 7).
- "The tag database contract" is `docs/contracts/tag-database.md`, version
  1.0. Its layout version 1 is frozen (its section 14.1): nothing in this
  contract changes a shared structure, a stored enumeration, a quality flag
  or a limit that readers validate. Where the survey asked for one, this
  contract gives the workaround and says so.
- Times are as in the tag database contract: `*_time_ns` is
  `CLOCK_REALTIME` (UTC), `*_mono_ns` is `CLOCK_MONOTONIC`. Every timeout,
  age, deadline and period in the framework uses monotonic time.
- A **deadline** is a monotonic time after which a blocking call must have
  returned. Every call that may block takes one, and the framework enforces
  it as the bound of section 2.3.

## 2. Scope, threads and the `ProtocolDriver` calls

### 2.1 What the framework does and what the protocol does

| Concern | Framework (`libscada-driver`) | Protocol |
| --- | --- | --- |
| Configuration | Loads the validated driver files, expands templates, resolves tag handles, owns the common part of each point (section 4) | Validates and compiles its own binding blocks (`configure`) |
| Tag database | Attach, heartbeat, notify, every tag write, the restart rewrite (section 9.1), the generation change | Never calls `libscada-tag`; never sees a handle |
| Scheduling | Poll groups, priorities, write-before-read, pacing, keep-alive, back-off, scan groups (section 6) | Plans blocks inside the framework's rules; runs one transaction when called |
| Samples | Applies scaling to engineering units, invalid markers, quality rules, STALE, writes tags (section 5) | Decodes frames into raw values and per-point outcomes |
| Failure | Counts failures, demotes and recovers devices, startup grace (section 7) | Classifies each outcome (timeout, exception, refusal) |
| Commands | Takes requests, every check of section 8.2, selections without native support, expiry, rate limits, results | Encodes and sends a write, a native select, operate or cancel; classifies the answer |
| Diagnostics | Publishes every diagnostic tag (section 10) | Reports counts and states through the framework |
| Reload | Decides live, per device, or generation change (section 11) | Accepts a new compiled binding for one device or refuses it |

Protocol specifics do not pass the framework (reference 3.3): above the
driver there are only tags and write requests. No type of a protocol library
appears in this interface (reference 16.3); a protocol wraps its stack.

### 2.2 Threads

A driver process has these threads. Decision D-82 records the threading
model; reference 6.1 fixes one worker per channel.

| Thread | Count | Runs | Tag owner | Real-time |
| --- | --- | --- | --- | --- |
| Main | 1 | Attach, heartbeat, the generation change, reload, the channel watchdog (2.5), process-level diagnostics | `drv-<protocol>` (its diagnostic tags) | No |
| Command | 1 | Takes every request from the `cmd` queues the process consumes (`scada_queue_next`), the checks of 8.2 that need no device, dispatch to the channel workers, and every answer (`result` queues) | None | No |
| Channel worker | 1 per channel | Connect, poll, activities, writes, report handling for that channel; every tag write of the channel's devices; `scada_tag_notify` after each scan | `drv-<protocol>.<C>` | Yes, for a channel with a fast poll group (2.4) |
| Protocol helper | 0 or more per channel, only where a third-party stack owns its own thread (2.6) | The stack's receive loop | None; hands samples to its worker (2.6) | No |

- One owner per channel worker matches the tag database: an owner is one
  writing thread (its section 2), and one channel is one owner (reference
  5.1, "one per driver channel").
- A `cmd` queue has exactly one consuming thread (tag database contract,
  section 9), and the answers are posted by the thread that takes the
  requests (its section 9.2). The command thread is that thread for every
  `cmd` queue of the process: one thread may consume many queues. The tag
  set gives one request queue per channel and requester thread,
  `cmd.<C>.<source>` with its answer queue `result.<C>.<source>`, where
  `<C>` is the channel's short name and `<source>` is `cmd` or the short
  name `<T>` of a control task (configuration schemas contract, 12.3):
  a queue has one producing thread, and `scada-control` has one per task.
  A full queue of one channel then refuses only that channel's requests.
  Reference 5.1 names the queues `cmd.<driver>.<source>`; the naming is
  the only difference (section 16.2).
- Process and owner names are those of the tag set (configuration schemas
  contract, 12.3): the process `drv-<protocol>` (service user
  `scada-drv-<protocol>`), the main thread's owner `drv-<protocol>`, one
  owner per channel `drv-<protocol>.<C>`. `<C>` is a short name generated
  from the channel's name alone (the first 10 hex digits of SHA-256 of
  `channel:` and the name), so that owner and queue names stay within
  31 bytes while channel names may have 31; `scada-conf` keeps the map of
  generated names. `scada-io` follows the same rule (owner `io`, queues
  `cmd.io.<source>`). Tag names use the full channel name (section 10).
- Between the command thread and each worker there are two pre-allocated
  single-producer, single-consumer lock-free queues of the framework: orders
  to the worker, outcomes back. Their capacity is the channel's
  `max_pending_writes` (default 16). A full order queue is answered at once
  `REJECTED` with reason `BUSY` (section 8.7): it is never waited on.
- The worker never calls `scada_queue_next`, which is not real-time safe in
  tag database 1.0 in any state (its section 9.5, #116). Only the command
  thread does. This answers #116 point 5 for drivers.

### 2.3 The interface

```cpp
namespace scada::driver {

class ProtocolDriver {
public:
  virtual ~ProtocolDriver() = default;

  // Main thread, before attach and at reload. May allocate; no I/O.
  // Validates every binding block and compiles it into the protocol's own
  // form, kept in Device and Point (section 4). Errors go to `errors`,
  // each with file, line and a code of section 4.3.
  virtual Status configure(const DriverConfig& config, ConfigErrors& errors) = 0;

  // Main thread, after configure. Static facts per device and point that
  // the framework schedules and checks by (section 2.7).
  virtual Capabilities capabilities(const Device& device) const = 0;

  // Main thread, after configure. Groups the points of one poll group or
  // activity into requests within the framework's read rules (section 6.3).
  virtual Status plan(Device& device, const ReadRules& rules, ReadPlan& out) = 0;

  // Channel worker. Opens the port or session. Blocks at most until
  // `deadline`. One call per connection attempt.
  virtual Status connect(Channel& channel, Deadline deadline) = 0;
  virtual void   disconnect(Channel& channel) noexcept = 0;

  // Channel worker, after connect and before the first poll of a device
  // (section 3.4). Each has a default that does nothing and succeeds.
  virtual Status resolve(Device& device, Deadline deadline);            // room: symbolic addresses
  virtual Result<Identity> identify(Device& device, Deadline deadline); // protocol identity objects
  virtual Status session_step(Device& device, SessionEvent event,
                              Deadline deadline);                       // room: login, interrogation

  // Channel worker. One scheduled read: a poll group, or a registered
  // activity (section 6.1). Delivers through `out`; blocks at most until
  // `deadline`.
  virtual Status poll(Device& device, PollGroup& group, SampleSink& out,
                      Deadline deadline) = 0;
  virtual Status run_activity(Device& device, Activity& activity,
                              SampleSink& out, Deadline deadline);      // default: NOT_SUPPORTED

  // Channel worker, report-driven protocols only (section 2.6). Called
  // when a descriptor the protocol registered is ready or a timer it set
  // is due. Must not block.
  virtual void on_event(Channel& channel, ChannelEvent event, SampleSink& out); // default: none

  // Channel worker. Starts one order (DIRECT, SELECT, OPERATE, CANCEL) on
  // the device. Completes through `done`, at once or later (section 8.6).
  virtual void write(Device& device, const WriteOrder& order,
                     WriteCompletion& done, Deadline deadline) = 0;

  // Channel worker. One read of one point outside its poll group.
  virtual Status read_now(Device& device, PointRef point, SampleSink& out,
                          Deadline deadline) = 0;
};

}  // namespace scada::driver
```

**open: needs the product owner's ruling (X-10).** Reference 6.1 shows seven
calls: `poll` is the only scheduled one, `connect` returns a `Status` and
`write` one `Result`. Adding `resolve`, `session_step`, `run_activity`,
`on_event` and a completion for `write` in phase 9 would change every
driver. This draft makes the change now, with defaults, so a phase 2
protocol implements the seven calls of the reference plus `capabilities` and
`plan`, and inherits the rest. The reference names are kept, and the
reference's `on_unsolicited` is `on_event`.

Every call, its thread, what it may block on, and whether it is real-time:

| Call | Thread | May block on | Bound | Real-time | Phase 2 protocol |
| --- | --- | --- | --- | --- | --- |
| `configure` | Main | Nothing; allocates | None (not in a cycle) | No | Must |
| `capabilities` | Main | Nothing | None | No | Must |
| `plan` | Main | Nothing; allocates | None | No | Must |
| `connect` | Worker | The connection or port open | `deadline`: the channel's `connect_timeout` (default 3 s) | No; a fast channel connects outside its cycle | Must |
| `disconnect` | Worker | Nothing beyond a close | 100 ms | No | Must |
| `resolve`, `identify`, `session_step` | Worker | Transactions on the device | `deadline`: per call, from the device's `timeout` and retries | No | Default (identify: may) |
| `poll`, `run_activity`, `read_now` | Worker | One transaction or a planned sequence of them, on its channel only | `deadline`: the device's `timeout` per transaction, times the transactions planned | Its decode and delivery parts are (2.4) | `poll`, `read_now`: must |
| `on_event` | Worker | Nothing | Returns before the next ready descriptor is served | Its decode and delivery parts are (2.4) | Default |
| `write` | Worker | The transactions of the order, or nothing when it completes later | `deadline`: the earlier of the request's expiry and the device's `write_timeout` | Its encode part is (2.4) | Must; completes at once |

- No call may block on a lock that another thread can hold for longer than
  a copy of a small structure, on a `libscada-tag` call, on the command
  thread, or on another channel.
- A call that passes its deadline is a protocol fault: the framework counts
  it (`deadline_overruns`, section 10) and, at three times the deadline,
  the channel watchdog of 2.5 applies.
- `configure`, `capabilities` and `plan` are called again for the devices a
  reload changes (section 11), on the main thread, while the worker of that
  channel is paused at a scan boundary.

### 2.4 Real-time rules for a fast poll group

A driver with a fast poll group is part of the 10 ms tier (reference 4.3).
Its worker runs under `SCHED_FIFO` with locked, pre-faulted memory when the
tier is available, and under the normal scheduler at the same period when
it is not (reference 4.3, point 3; the fallback alarm is the process's own
system tag `sys.proc.<process>.realtime`, section 10).

**open: needs the product owner's ruling (R-3).** `CLAUDE.md` says that code
running in "the I/O scan" must not "make system calls ... or do file or
network I/O". An I/O scan cannot exchange a frame with a device without the
system calls of that exchange, so the rule cannot hold for the scan as a
whole. This draft proposes the reading below (decision D-83). `CLAUDE.md`
and the coding standard are not changed by this contract.

A scan of a fast channel is a sequence of parts:

1. **Take orders** from the in-process order queue (lock-free, no system
   call). `SCADA_RT`.
2. **Transactions:** the protocol's send and receive through its port, on a
   descriptor opened outside the cycle, each bounded by its deadline, with
   no allocation, no logging and no lock. Not `SCADA_RT`.
3. **Decode and deliver:** the protocol's decode into the `SampleSink`, the
   framework's scaling, quality rules and `scada_tag_write_many`. `SCADA_RT`.
4. **Outcomes** to the command thread through the lock-free outcome queue.
   `SCADA_RT`.
5. **Notify:** `scada_tag_notify` once per scan, after the parts above
   (reference 5.3). Not `SCADA_RT`.

The calls that enter the kernel on a real-time worker, and nothing else:

| Call | Part | Why | Bound |
| --- | --- | --- | --- |
| `send`/`write` on the device descriptor (non-blocking) | 2 | The request frame | One call per frame; returns at once (`EAGAIN` is a protocol fault, counted) |
| `ppoll` on the device descriptor and the worker's eventfd | 2 | Wait for the answer | The transaction's deadline: the device's `timeout`, at most the scan period of the fast group |
| `recv`/`read` on the device descriptor | 2 | The answer frame | One call per readiness, non-blocking |
| `ioctl` of the serial line (RTU only: drain, line control) | 2 | Turnaround on a half-duplex line | The frame time at the baud rate plus the configured turnaround |
| `read` of the eventfd | 1 | Clear the order signal | Non-blocking, one call per scan |
| `clock_gettime` | All | Deadlines and times | vDSO; a real system call only on the clock sources of tag database contract 13.1, which reports it |
| `scada_tag_notify` (futex wake; once a second a look with `fstatat`, and possibly `open`, `mmap` and a populate; once a minute the clock check) | 5 | Wake consumers | Bounded but not real-time safe (tag database contract, section 13; #116 point 3) |

No other system call is made on the worker after its first scan: connect,
reconnect, configure, reload and detach run outside the cycle (2.3, 9.2).

What the `SCADA_RT` check catches under this reading, and what it does not:

- **Caught:** allocation, locks, logging, exceptions, `std::function`,
  string construction and calls to non-RT-safe functions inside parts 1, 3
  and 4 and every function they call.
- **Not caught:** anything in parts 2 and 5, which are not annotated. The
  bound of part 2 is checked by the deadline at run time
  (`deadline_overruns`, section 10) and by review of each protocol's
  transaction code; the scan's total time is measured by the cycle
  statistics of reference 4.3, point 4. That no other system call is made
  is checked by conformance test 29 (a system-call trace of the worker),
  not by the annotation.
- So the annotation's placement shows where the gap is: the transaction
  functions of every protocol are a reviewed, unannotated list
  (`DRIVER_IO_FUNCTIONS` in the protocol's README), not code the check has
  passed.

`SampleSink`, `WriteCompletion` and every buffer a fast scan uses are
allocated at configure and sized from the plan: delivery never grows a
container (coding standard, section 11). A protocol that cannot meet this
declares `realtime_capable = false` in its capabilities, and the
configuration is refused when it is given a fast poll group (code
`DRV-RT-01`).

Phase 2: the Modbus RTU and TCP protocols and `scada-io` must be real-time
capable. The rule above is checked by the `SCADA_RT` check on parts 1, 3
and 4.

### 2.5 Channel watchdog

The heartbeat is per process (tag database contract, section 10.1). A
worker that stalls while the main thread beats would leave its tags with
their last values and real age, never OWNER_DOWN (#116, comment point 1).
The framework closes that gap for drivers:

- Each worker increments a progress counter (an atomic in process memory)
  at every scan boundary and at every return from a protocol call.
- The main thread checks every counter at each heartbeat. A worker whose
  counter has not moved for `stall_limit` (default: three times the longest
  deadline the worker can be inside, at least 1 s) is stalled.
- On a stall the main thread logs the channel, stops calling
  `scada_tag_heartbeat`, and lets the process be seen down (every tag of the
  process reads BAD with OWNER_DOWN within the heartbeat timeout) and
  restarted by systemd. It never kills or cancels a thread.

Decision D-84. Phase 2.

### 2.6 Report-driven protocols

Decision D-82 (D-40 of the survey, an open decision of the outline).

- **Default: the worker is an event loop.** A report-driven protocol
  registers its descriptors and timers with its `Channel` at `connect`. The
  worker waits on them, on the order queue (an eventfd the command thread
  signals) and on the next scheduled poll, and calls `on_event`, `poll`,
  `run_activity` or `write` for whichever is due. So the protocol receives
  while a write awaits its termination, without a second thread.
- **A stack with its own thread** (some third-party stacks run their own
  receive loop) runs it as a protocol helper thread. The helper never
  writes tags and never calls the framework except through the channel's
  `SampleSink` in its thread-safe form (`Channel::remote_sink()`): a
  bounded single-producer queue, drained by the worker in `on_event`. A
  full queue drops the samples and counts them (`samples_dropped`), and the
  framework treats the drop as a buffer overflow (section 5.7). The helper
  completes writes through the same queue.
- A worker of a report-driven channel is not real-time capable unless its
  protocol declares it and passes the rules of 2.4.

Reason: one writer per owner keeps the tag database's single-producer rule
without a lock; the event loop is the one model that serves a polled
protocol and a report-driven one with the same worker, and the helper form
admits stacks that insist on a thread without letting them touch the tag
database.

Phase 2: the worker of the Modbus channels runs the loop with no protocol
descriptors (polled only). The event-loop part of the interface is room.

### 2.7 Capabilities

`capabilities(device)` returns static facts the framework relies on. A
default exists for each so a phase 2 protocol sets only what applies.

| Fact | Per | Values | Default | Used by |
| --- | --- | --- | --- | --- |
| `realtime_capable` | Protocol | bool | false | 2.4 |
| `acquisition` | Point | `poll`, `report`, `both` | `poll` | 6.1, 7.4 |
| `select` | Point | `gateway`, `native` | `gateway` | 8.4 |
| `native_select_window` | Point with native select | duration | none | 8.4 |
| `write_completion` | Point | `immediate`, `two_phase` | `immediate` | 8.6 |
| `idempotent_write` | Point | bool | false | 8.7 |
| `max_read_size`, `max_write_size` | Device | count of protocol units | protocol limit | 6.3 |
| `needs_resolve` | Device | bool | false | 3.4 |
| `activities` | Device | list of activity kinds with default periods | none | 6.1 |
| `time_sync` | Device | none, or the clock forms the protocol can set | none | 6.8 |

A point the protocol marks `native` must also give its window. The
framework refuses (code `DRV-SBO-01`) an output configured `sbo` on a point
whose select is `native` but whose window the configuration cannot honour
(section 8.4).

## 3. Object model

### 3.1 Objects

| Object | What it is | Created | Destroyed | Owned by |
| --- | --- | --- | --- | --- |
| `Driver` | The process's framework instance and its `ProtocolDriver` | Process start | Process exit | `main` |
| `Channel` | One connection or port: a serial line, a TCP connection, a session; its worker | `configure` | Reload that removes it, or exit | `Driver` |
| `Device` | One addressable device on a channel, with its own timeout, retries, demotion state, identity and session state | `configure` | Reload that removes it, or exit | Its `Channel` |
| `Point` | One configured signal of a device, bound to one tag (or, for a command-only point, to none) | `configure` | With its device | Its `Device` |
| `PollGroup` | A named period and priority on a channel, with the points assigned to it | `configure` | With its channel | Its `Channel` |
| `Activity` | A scheduled non-poll task of a device (section 6.1) | `configure` | With its device | Its `Device` |
| `Output` | The write side of a point: kind, encoding, select model, limits, refresh, shutdown behaviour (section 8) | `configure` | With its point | Its `Point` |

- Every object is created on the main thread and used afterwards by the
  worker of its channel only, except for the read-only parts the command
  thread uses for its checks (the `Output` configuration, published to it
  at configure and replaced only at reload).
- A protocol keeps its compiled form of a device or point in a slot the
  framework gives it (`Device::protocol_state<T>()`, a `std::unique_ptr`
  owned by the object). It never keeps a pointer to a framework object
  beyond the call that passed it, except to the `Channel`, `Device` and
  `Point` objects themselves, which live until reload or exit.

### 3.2 One point is not one address

The survey's devices break "one point, one address, one scalar" (survey
D-1). The model allows:

- one read delivering samples for many points (block reads, section 6.3);
- one point needing several protocol units, in a stated order, possibly not
  adjacent (a 32-bit value from two separate registers);
- one point with companions it depends on: an exponent, a status or
  validity source, a time source (section 5.5). The planner never separates
  a point from its companions (section 6.3);
- one read delivering several bit or field points from one word (section
  5.6);
- one command writing several protocol units, as an ordered list of writes
  (section 8.3).

### 3.3 Devices on a channel

- Several devices share one channel. Each has its own timeout, retries,
  demotion state, health and diagnostics. The worker serialises their
  transactions: a timeout on one device delays the others by one
  transaction at most (survey D-3).
- A lost connection fails every device of the channel at once (section
  7.2), without spending each device's retries.
- A channel is one connection per device group, never one per poll group.
- **open: needs the product owner's ruling (X-3).** A device that expects a
  main and a backup connection, or one that has two addresses: this draft
  proposes that a channel owns a list of endpoints (option (a)), one of
  them active, and that a device stays under one channel. Phase 2 accepts
  one endpoint. Room.
- **open: needs the product owner's ruling (X-4).** Address rules on a
  channel: this draft proposes that a device has a default protocol address
  (a Modbus unit id), that a point's binding may name another (one device
  answering on several unit ids, option (c)), and that several devices may
  share a unit id when their point addresses do not overlap (slices of one
  gateway unit, option (a) for slices). The overlap rule of reference 12.5
  then applies to points, not to devices. Phase 2.
- **open: needs the product owner's ruling (X-2).** Direction and role: the
  channel has `role: connect | listen` (who opens the connection); phase 2
  accepts only `connect`,
  and listening channels are room. Every driver stays client or master
  (reference 6.4); a server role is out of scope (reference 1).
- Other masters may share a device (the survey found two to seven clients
  on relays): shared selectors and event lists, blocks locked by another
  client, commands of another master arriving as feedback. The framework
  takes the feedback tag as the truth and never assumes it is the only
  writer (section 8.9).

### 3.4 Device state

Each device has a state, published as `sys.dev.<device>.state` (INT64,
section 10):

| Value | State | Meaning | Tags of the device |
| --- | --- | --- | --- |
| 0 | `DISABLED` | Disabled in configuration | Its tags exist (the tag set is the same enabled or not): points BAD with NOT_INIT, diagnostics written, `state` 0; nothing is sent to the device |
| 1 | `CONNECTING` | Channel not connected | BAD, COMM_FAIL (after the first attempt) or NOT_INIT (before) |
| 2 | `INITIALISING` | Connected; resolve, identify and session steps running, or inside `startup_grace` | Kept; BAD with NOT_INIT if never read |
| 3 | `RUNNING` | Polled or reporting | From samples (section 5) |
| 4 | `DEMOTED` | Consecutive failures reached the limit; retried at back-off (section 7.3) | BAD, COMM_FAIL |
| 5 | `MISMATCH` | Identity check failed with `on_mismatch: refuse` (section 3.5) | BAD, no COMM_FAIL; writes refused |
| 6 | `REFUSING` | Reachable but refusing: answers that say the server function is off, a login failed, or the device is not started | BAD, no COMM_FAIL |

Transitions are made by the framework only. `INITIALISING` runs, in order,
`resolve` (if `needs_resolve`), the identity check (section 3.5),
`session_step(CONNECTED)` and, for report-driven protocols,
`session_step(INTERROGATE)` until the protocol reports the end of the
interrogation; points stay NOT_INIT until their first sample. A detected
device restart (section 7.5) goes back to `INITIALISING`.

Phase 2: states 0 to 5. `REFUSING` is entered by a Modbus device only
through the refusals of annex A, row "gateway target".

### 3.5 Identity

- Configured identity (survey C-45, D-7): a list of points or protocol
  identity objects with an expected value, list, range or pattern, and
  `on_mismatch: warn | refuse` (default `warn`).
- The framework reads configured identity points with `read_now`, and
  calls `identify` for protocol identity objects, after every connect and
  after a detected device restart. It compares, publishes
  `sys.dev.<device>.identity` (STRING, at most 250 bytes, the values read) and
  `sys.dev.<device>.identity_ok` (BOOL), and on a mismatch with `refuse` sets
  the device `MISMATCH`.
- `MISMATCH` is left only by a reload of that device or a later check that
  matches. A device in `MISMATCH` answers every write `REJECTED` with
  reason `CONFIG_MISMATCH` and is polled only for its identity points, at
  the back-off rate.

Phase 2.

## 4. Point addressing

### 4.1 The binding block

A point has common keys, defined by the configuration schemas (#98) and
processed by the framework, and one protocol key, `address`, opaque to
the framework. Channels and devices follow the same rule: framework keys
and protocol keys sit side by side in one object, the two sets are
disjoint, and an unknown key is an error. In this contract the protocol
part of a point, device or channel is called its **binding**.

| Part | Contents | Validated by |
| --- | --- | --- |
| Common keys | Tag name, tag type, kind (plain, bit, enumeration, double point, counter), unit, label, `scale`, `invalid_values`, `on_invalid`, `quality_from`, companions (`exponent_from`, status, time source), `bits`, `enum`, counter rules, `max_age`, `poll_group`, `access` (`ro`, `rw`, `wo`), `side_effect`, `acquisition`; alarms and logging, which the driver does not use | The schema (#98), then the framework |
| Binding (`address`, and the protocol keys of devices and channels) | Address, raw encoding (raw type, byte and word order, bit or field, register count), write encoding (8.3), protocol status mapping, read rules (`read:`), protocol options | The protocol's schema fragment (#98, annex A there for Modbus), then the protocol at `configure` |

- Each protocol ships its schema fragments: channel, device, address and
  read rules. The configuration schemas contract says how they are merged
  into the file schemas; this contract says that `configure` validates the
  binding again (4.2).
- **open: needs the product owner's ruling (X-8).** Reference 12.3 puts
  `protocol: modbus` at the top of a template with addresses inline. One
  device type can have four address maps and four raw scalings, one per
  protocol. Both drafts propose option (b) for phase 2: a template carries
  one `protocol:` and may `extends:` a protocol-free base that has common
  keys only. Option (a), one template with a binding per protocol, is room.
  The driver side is the same under both: the framework hands the protocol
  its own binding only (section 15, B-2).
- The protocol receives each binding as a typed configuration tree
  (`ConfigNode`) that carries file and line for every value, never as text
  to parse.
- An instance may supply address data: a map from label to position,
  parameters used in address expressions, or a generated point list. The
  framework expands it before `configure` (#98); the protocol sees concrete
  bindings. Every enabled point is placed, and no position is used twice
  within a device's address space unless the binding says the two points
  read the same unit (bit points of one word).

### 4.2 What `configure` does with a binding

- Checks every field against the protocol's limits and the device's
  declared sizes, and compiles the binding into its own form, kept in the
  point's protocol slot.
- Refuses with a located error, never by a default: an unknown field, a
  value out of range, a raw type that cannot hold the tag type, a byte
  order the raw type does not have, a bit beyond the raw width, overlapping
  addresses that are not declared as one unit.
- Reports, for each point, its dependencies on other points (section 5.5)
  and the raw unit count it reads.
- A symbolic address that needs the device (an address found by browsing,
  a model chain) compiles to "unresolved"; `resolve` turns it into a
  concrete one after connect (room, survey D-6). A point still unresolved
  after `resolve` is "absent" (section 5.2, outcome 3).

### 4.3 Error codes

Codes of the driver side, cited by the validation chapter of #98. Each
error names file, line, the device and the point.

| Code | Meaning |
| --- | --- |
| `DRV-CFG-01` | A binding field the protocol does not know |
| `DRV-CFG-02` | A value out of the protocol's range |
| `DRV-CFG-03` | Raw type and tag type do not fit (section 5.4) |
| `DRV-CFG-04` | Overlapping addresses not declared as one unit |
| `DRV-CFG-05` | A companion (exponent, status, time) on another device, table or poll group (section 6.3) |
| `DRV-CFG-06` | A read rule the protocol cannot honour (block size above its limit, an atomic group above the block size) |
| `DRV-CFG-07` | A write encoding the protocol cannot send, or a function the device's `supported_functions` excludes |
| `DRV-CFG-08` | A `wo` point in a poll group, or a `side_effect` point in more than one poll group or answerable by `READ_NOW` |
| `DRV-RT-01` | A fast poll group on a protocol or channel that is not real-time capable |
| `DRV-SBO-01` | A select model the point cannot honour, or an `at_operate` output whose device window is below twice the device timeout (section 8.4) |
| `DRV-SBO-02` | A select timeout longer than the device window with `native_select: at_select`, or than the 60 s request lifetime |
| `DRV-PACE-01` | A poll group that cannot be served at the channel's pacing (section 6.4): a warning (X-13) |
| `DRV-PACE-02` | Slowest poll period and back-off above the device's idle timeout, with no keep-alive: a warning |
| `DRV-NVM-01` | An output with `storage: nvm` and a cyclic source (`refresh`, or a control block) |

## 5. Samples and quality

### 5.1 What a protocol delivers

A protocol delivers **raw samples** to the `SampleSink`. A raw sample
names a point and carries:

- one outcome (5.2);
- the raw bit pattern as received, up to 64 bits (for invalid markers,
  5.3), and the raw number the protocol decoded from it with the binding's
  raw type and byte and word order (an integer or a float), or a byte
  string for a string point;
- an optional device time and its kind (5.8);
- an optional protocol status code (an `int32`);
- an optional cause (5.9) and a replay mark (5.9).

The framework turns a raw sample into a tag value:

1. Invalid markers on the raw pattern, before any decoding (5.3).
2. The protocol's raw scaling and exponent, then the common engineering
   scaling (reference 5.2: scaling is applied before writing). Decimal
   gains are applied as configured, and the result type follows 5.4.
3. Bit and field extraction, enumeration and double-point mapping (5.6).
4. Quality from the outcome, the protocol status mapping, companions and
   the raw range (5.2, 5.5).
5. `source_time_ns` and the time flags (5.8).
6. The write, one `scada_tag_write` per sample, or one
   `scada_tag_write_many` per delivery.

The sink accepts several samples of one point in one delivery, in the order
given. Each is written in turn, so the change ring, alarms and the
historian see each of them (a trip pulse shorter than a poll; survey D-11).
The owner's change ring must hold the largest burst a device can send: the
ring is at least twice the owner's tag count (tag database contract,
section 8); a channel whose devices can send larger bursts asks for a larger
ring in configuration (#98).

### 5.2 Outcomes

Every raw sample has one of these outcomes. They are distinct in the
interface, so that the framework can count and act on each.

| # | Outcome | Meaning | Quality | Flags set | Demotion |
| --- | --- | --- | --- | --- | --- |
| 1 | `VALUE` | The device answered with a value | GOOD, unless 5.3, 5.5 or the protocol status mapping lower it | As those rules give | Clears the failure count |
| 2 | `NO_VALUE` | The device answered, but the value is a "no value" marker (an invalid marker of 5.3 with `quality: bad`) | BAD | The marker's flags (default none) | No |
| 3 | `ABSENT` | The point is not on this device: the device refused the address (a Modbus exception "illegal address" on a configured point), or `resolve` did not find it | BAD | NOT_INIT if never read, else none | No |
| 4 | `DEVICE_INVALID` | The device reported the value invalid: a status code, a validity bit, a health rule, a protocol quality | BAD or UNCERTAIN, from the mapping | The mapping's flags: BLOCKED for out of service or blocked, TEST for test or simulation, OUT_OF_RANGE, OVERFLOW, SUBSTITUTED for a value forced at the device | No |
| 5 | `COMM_FAIL` | No usable answer: timeout, broken frame, lost connection, a gateway that cannot reach its device | BAD | COMM_FAIL | Counts (section 7.3) |

- The value of a tag whose sample is not `VALUE` is the last good value
  when there was one, otherwise 0. Readers must not use the value of a BAD
  tag (tag database contract, section 6.2).
- An `ABSENT` point is counted in `sys.dev.<device>.points_absent`, gets a
  configuration diagnostic (logged once per point and connection), and is
  requested again only at the back-off rate, not at its poll rate.
- `NO_VALUE` and `DEVICE_INVALID` are counted per device
  (`sys.dev.<device>.sentinels`, `sys.dev.<device>.invalid`).
- The protocol status code travels with the sample to the counts and to
  `sys.dev.<device>.last_status`; it is not stored in the tag.

What does not fit the frozen tag database (tag database contract, section
14.1), and how this contract carries it:

| Detail | Carried as |
| --- | --- |
| Protocol quality detail without a flag (oscillatory, inaccurate, inconsistent, failure, time accuracy) | Folded into GOOD, UNCERTAIN or BAD by the protocol's status mapping; the raw quality is available as a companion tag (UINT64) when the binding asks for one (room; #119) |
| "Not implemented", "not on this variant" | No tag where configuration knows it (a disabled point); otherwise `ABSENT` |
| Device disabled, failed, in local mode | `DEVICE_INVALID` with BAD and no COMM_FAIL, plus the device's health tags (5.5) |
| Configuration or permission fault | `REFUSING` device state (3.4) and `sys.dev.<device>.last_status` |
| Strings over 250 bytes | Truncated to 250 bytes at a UTF-8 boundary, with OVERFLOW |

### 5.3 Invalid markers

- A binding may list invalid markers: raw patterns compared before
  decoding, each with quality, flags and a reason. A float marker may be an
  exact pattern or "any NaN".
- The default is none. A template may set defaults per raw type; a point
  may override them.
- `on_invalid: bad | uncertain | keep_last | { substitute: <value> }`
  chooses what a marker does; `bad` is the default. `keep_last` keeps the
  last value with UNCERTAIN; `substitute` writes the value with
  SUBSTITUTED and UNCERTAIN.
- A raw range (`raw_range`) sets OUT_OF_RANGE with UNCERTAIN, for devices
  that saturate silently.

Phase 2.

### 5.4 Tag types

- A raw integer with an integer gain and offset stays INT64 or UINT64; any
  other scaling gives FLOAT64 (exact for integers up to 2^53).
- Bits are BOOL. Enumerations are INT64 with the table in configuration.
- Double points are INT64 with one coding for every driver (survey section
  7): 0 intermediate, 1 open or off, 2 closed or on, 3 bad state. 3 gives
  BAD with no flag; 0 stays GOOD, so that a travelling switch is not
  mistaken for a failure. A protocol maps its own coding onto this.
- Times decoded from the device (an epoch, a field set) are INT64
  nanoseconds UTC.
- Counters: `kind: counter` with `limit`, `at_limit: wrap | saturate` and
  `on_decrease: reset | hold | accept`. A wrap or a saturation sets
  OVERFLOW on that sample.
- Strings are STRING tags, at most 250 bytes (tag database contract,
  section 7).

### 5.5 Companions and health

- A point may depend on companions: an exponent from another raw unit, a
  status word or validity bit, a time source. The binding declares them;
  `configure` reports them (4.2). The planner reads a point and its
  companions in one request (6.3). A companion read with any outcome but
  `VALUE` makes the dependent sample BAD with that outcome's flags.
- `quality_from` maps a companion's bit, field or value to the quality of
  a point.
- A device `health` rule applies to all its points or a named group, and
  states whether it sets COMM_FAIL (a device lost behind a gateway: yes; a
  device that reports itself disabled: no).

Phase 2.

### 5.6 Bits, fields and enumerations

- One raw unit may deliver several bit or field points (`bits: [lo, hi]`);
  only named bits create tags.
- The bit numbering of a binding is stated (`bit_order: lsb0 | msb0`, per
  template); there is no default the protocol may assume.
- A change latch (a bit that records "changed since last read", cleared on
  read): this contract exposes it as its own BOOL tag, written each poll
  (decision D-87, survey D-21). The framework does not synthesise an
  on-then-off event from a latch and a state that has returned: the time of
  such an event would be invented. Synthesis is room for a later protocol
  option, and would carry LOCAL_TIME (5.8). The latch point is a
  `side_effect` read (6.6).

### 5.7 Buffer overflow and lost events

A protocol that learns that the device dropped events or that its own
buffer overflowed (a report overflow, an overflow indication, a full helper
queue) calls `SampleSink::events_lost(device, count_or_unknown)`. The
framework counts it in `sys.dev.<device>.events_lost`, logs it, and runs the
device's integrity read (an interrogation, or one full poll) so that the
current state is read again. Room; phase 2 calls it only for a full helper
queue, which Modbus does not have.

### 5.8 Time of a sample

| Case | `source_time_ns` | Flags |
| --- | --- | --- |
| Polled point, no device time (every Modbus point in phase 2) | The receive time of the answer | LOCAL_TIME |
| Device time of the change, device clock synchronised | The device time, converted to UTC | None |
| Device time with the device clock unsynchronised, or of a device configured `device_time: { zone: local }` whose zone the unit cannot confirm | The device time, converted | TIME_UNCERTAIN |
| An event whose device gave no time, stamped by the driver | The receive time | LOCAL_TIME |
| The unit's own clock is not synchronised (reference 14.4) | As above | TIME_UNCERTAIN added |

- **Driver-stamped event time** (the reports differed): LOCAL_TIME, because
  that is the flag's meaning in the tag database ("`source_time_ns` was set
  by the owner, not a device", its section 6). TIME_UNCERTAIN keeps its one
  meaning: a clock that may be wrong. Decision D-86.
- A device configured with local time (`device_time: { zone, tz }`, #98) is
  converted by the framework with the configured zone.
- The unit's synchronisation state is read from the system tag that the time
  service will publish (reference 10.4, phase 7). Until it exists the
  framework does not add TIME_UNCERTAIN for the unit's clock. Room.
- For a polled point an unchanged value read again is a fresh sample: the
  write refreshes `update_time_ns` and the age, and clears STALE.

### 5.9 Replayed events and cause

**open: needs the product owner's ruling (X-6).** After a reconnect a
device may replay buffered events; then an interrogation gives the current
state. The reports differed. This draft proposes option (a), with a marking
that uses an existing flag, decision D-88:

- A protocol marks each sample it knows to be replayed (a buffered report
  sent again, an event older than the connection). Every replayed sample is
  written to its tag, in device-time order within the delivery, with its
  device time, and with **STALE added** to its quality flags.
- The device stays `INITIALISING` (3.4) until the protocol reports the end
  of the interrogation. The interrogation writes the current state last,
  without STALE, so the live tag is current once the device is `RUNNING`.
  A point the interrogation does not cover keeps STALE until its next
  confirmation (7.4).
- STALE is the honest mark: a replayed value is not proven current, which
  is what STALE says for every tag (7.4, X-11). No new flag is needed, and
  RESTORED (a retained value, tag database contract section 12) is not
  used.
- Reason: the historian, alarms and the journal learn values only through
  change rings (tag database contract, section 8). Option (b), "historian
  only, live tag only when newer", needs a path that bypasses the tag, or
  a quality bit: a new path, or a layout version. Option (a) needs
  neither.

Consequences for consumers, which the reviewers are asked to test:

| Consumer | What it sees | What stops a replayed trip from acting |
| --- | --- | --- |
| Any reader | `source_time_ns` of one tag can go backwards between two writes; `update_time_ns` and `update_mono_ns` never do | Nothing needed; age uses `update_mono_ns` (reference 5.2) |
| Control blocks (phase 6) | A replayed transition is in the tag for at least the time until the next write, so a task may snapshot it | The sample carries STALE. Every block applies its bad-or-stale behaviour (reference 7.3: `hold`, `safe_value` or `propagate`) to a STALE input, so an edge block does not fire and an output block holds. The phase 6 block library must treat STALE as stale on every input; a block that ignored it would act on an old trip. This is a requirement on phase 6 |
| Alarms (phase 7) | Replayed transitions with old source times, then the current state | The alarm engine must not raise a new alarm from a sample with STALE; it may journal the replayed transition as a past event with its source time. A requirement on phase 7, to be written into its contract |
| Historian (phase 3) | Samples with source times older than ones already stored, flagged STALE | The historian contract must state how it orders and stores them (by source time, with the flags kept) |
| Journal | Nothing from the driver | The cause and the replay mark go to the counts only, until a later contract gives them a path |

Without STALE on replayed samples, nothing would stop a replayed trip from
driving logic for one cycle: that is why the mark is part of the proposal,
not an option of it.
- The cause of a sample (spontaneous, cyclic, interrogated, requested,
  return information, test) is carried in the raw sample for the journal
  and the counts, not in the tag. Room: a later contract may give it a
  path to the journal.

Phase 2: Modbus has no replay; the rule applies from phase 9.

## 6. Scheduling

### 6.1 Poll groups and activities

- A channel has poll groups, each with a period and a priority (reference
  6.2). A point is in one poll group, or in none (a `wo` point, a
  report-only point).
- Besides poll groups a protocol may register activities per device in its
  capabilities (survey D-16): integrity and class polls, record readers,
  keep-alive, time sync, refresh. The framework schedules each by its
  period and calls `run_activity`. Room, except the framework's own
  activities of phase 2: keep-alive (6.5), identity (3.5) and back-off
  probes (7.3).
- Reported points (`acquisition: report`, room) are delivered by
  `on_event`; a reported point may also be in a poll group for an
  integrity read.

### 6.2 Order of service on a channel

At each scan boundary the worker serves, in order (reference 6.2):

1. Pending orders (writes, selects, operates, cancels, `READ_NOW`), in
   arrival order, within the device's write pacing (6.4).
2. Due poll groups and activities by priority, then by due time.
3. Back-off probes of demoted devices (7.3).

An order arriving while a poll group's planned sequence of transactions is
running waits for the end of the current transaction, not of the group.

### 6.3 Block planning

The framework gives `plan` the rules; the protocol builds the requests.

- Rules (survey C-37, D-17): `max_registers` (or the protocol's unit) per
  request, at most the device's `max_read_size`; `max_gap`, the largest
  unconfigured run the plan may read across (default 0); forbidden ranges
  (`no_read`) the plan never touches; `atomic` groups always in one
  request; exact-shape blocks read only as configured.
- The plan never splits a multi-unit value, never separates a point from
  its companions (5.5), never merges across tables or address spaces, and
  never merges a `side_effect` point with another.
- What a bridged gap returns is never decoded.
- The plan is published: `sys.dev.<device>.requests_per_scan` per poll group
  (UINT64) and, in the debug log at configure, each request's range.
- When a merged request fails with "illegal address" (annex A), the
  framework asks the protocol to split it once into its points' own
  requests; the points that fail again are `ABSENT` (5.2); the rest keep
  the split plan until reconnect.

Phase 2.

### 6.4 Pacing

- Per device and per channel: `min_request_interval`, `min_write_interval`
  and `max_values_per_request` (survey C-5). The worker never sends two
  requests to a device closer than its interval, and "writes first" keeps
  the write interval.
- A device paced at 10 s does not delay its neighbours: the worker serves
  other devices while one waits for its interval.
- The framework publishes the achieved period per poll group
  (`sys.drv.<channel>.period.<group>`, ms), requests per second per
  channel, and `sys.dev.<device>.pacing_limited` (BOOL), true while a group of
  the device misses its period because of pacing.
- **open: needs the product owner's ruling (X-13).** Whether an unservable
  poll group is a validation error or a warning is #98's to fix. This draft
  proposes a warning (`DRV-PACE-01`): the runtime comparison of measured
  and configured periods already raises an alarm (reference 7.5).

Phase 2.

### 6.5 Keep-alive

A device that closes an idle connection (survey C-6) gets `keepalive:
<period>`: when no request has been sent to it for the period, the worker
sends one, the protocol's cheapest read (for Modbus, the first request of
its slowest poll group). The back-off of a demoted device respects it.
Phase 2.

### 6.6 Side effects and exclusive sequences

- A `side_effect` point (a read that clears a flag, moves a pointer or
  restarts a statistic) is read by one poll group only, never merged, never
  retried transparently, and never read by `READ_NOW` (`DRV-CFG-08`).
- An exclusive sequence (write a selector, read, restore) runs on one
  device with no other transaction to that device between its steps. Room:
  record readers are activities (6.1) that run such sequences; phase 2
  marks the registers `no_read` or `side_effect` so that nothing else
  touches them (survey M-13).
- A second master on the same device can break either. The framework does
  not detect it; the template documents it.

### 6.7 Synchronous scan groups

A scan group (reference 6.2) is a set of devices on one channel. At the end
of each scan of the group the worker writes the group's scan counter, a
system tag `sys.drv.<channel>.scan.<group>` (UINT64, +1 per scan),
then calls `scada_tag_notify`. A control task bound to the group waits for
the driver's `cycle_seq` and then checks the counter. `cycle_seq` counts the
notify calls of the whole process (tag database contract, section 10), so
the counter is what tells the task that its own group has finished (#116,
comment point 4). No layout change. Phase 2.

### 6.8 Time sync of devices

**open: needs the product owner's ruling (X-12).** Who sets device clocks is
not in the reference. This draft proposes option (b) in shape and (a) in
phase 2: time sync is a framework activity per device, default off; it
writes the clock forms the protocol declares, in UTC or the device's zone,
and never while the unit's own clock is not synchronised. Phase 2 has none.
Room.

## 7. Failure handling

### 7.1 Timeouts and retries

- Each device has `timeout` (per transaction), `retries` (reads only) and
  `startup_grace` (#98, survey C-4).
- A read that times out is retried up to `retries` times within its poll
  group's turn. A write is never retried (section 8.7).
- A `write_timeout` may scale with the count of values written.

### 7.2 Connection loss

When the channel's connection is lost, every device of the channel goes
`CONNECTING` at once: its tags BAD with COMM_FAIL, no device spends its
retries. The worker reconnects with a back-off from 1 s doubling to
`reconnect_max` (default 30 s), within the keep-alive of any device. A
connection that succeeds while the devices refuse (a server function off)
puts them `REFUSING`, not `RUNNING`.

### 7.3 Demotion and recovery

- A device is demoted after `demote_after` consecutive failed transactions
  (default 3), counted across poll groups (reference 6.2).
- What counts: a timeout and a "gateway cannot reach its device" answer
  (annex A). What does not: "busy", a `NO_VALUE` or `ABSENT` answer, and
  any failure within `startup_grace` after connect or a detected restart.
- A demoted device: tags BAD with COMM_FAIL; polled no more; probed with
  one request every `backoff` (default 10 s, at most `backoff_max`); a
  probe that succeeds recovers it to `INITIALISING` and then `RUNNING`.
  A demoted device no longer spends its timeout every scan, so it stops
  stalling the channel.
- Demotion is per device. Several devices that share one address (3.3)
  each demote on their own failures.

Phase 2.

### 7.4 Staleness

**open: needs the product owner's ruling (X-11).** Reference 6.2: "If a
point is not refreshed within its `max_age`, its tag gets the `STALE`
flag." This draft reads "refreshed" as "confirmed":

- A polled point is STALE when no read of it has succeeded within its
  `max_age`.
- A reported point (room) is STALE when, within its `max_age`, there was
  neither a report of it, nor an integrity answer that includes it, nor
  link supervision that proves the session alive (a point that does not
  change sends nothing, and is not stale while the session is proven).
- An event-only point (it has no state the device reports in an
  interrogation) stays NOT_INIT until its first event, or takes a rest
  value from configuration; it is never STALE.
- STALE is added to the tag's last quality and value; the framework writes
  it once when the age passes `max_age`, checked at every scan boundary,
  and clears it with the next confirmation. A replayed sample is written
  with STALE (5.9). Marking staleness is the
  driver's job for its own tags (reference 6.2; tag database contract,
  section 6).

Phase 2: polled points.

### 7.5 Device restart

The framework or the protocol detects a device restart from a configured
indication (a run-time counter going backwards, a power-up flag, an
identity change, a protocol's restart indication). On a restart the device
goes back to `INITIALISING` (3.4): identity check, session steps, points
NOT_INIT until read, and device-side settings sent again (room). Phase 2:
the configured indications only.
## 8. Write requests

**Review by a person:** this whole section is command handling,
select-before-operate and fail-safe behaviour (`CLAUDE.md`, "Safety and
security rules").

Nothing in this section relaxes the tag database contract: requests are
delivered at most once, requests still queued at attach are answered ABORTED
by the library, expired and over-long requests are answered EXPIRED by the
library and never reach the driver, and every request needs its event
record accepted before it is posted (tag database contract, sections 9.2
and 9.3). The requester posts that record; the library does not check the
pairing, and neither does the driver, which cannot see the journal.

### 8.1 Requests and orders

The command thread takes a `WriteRequest` (tag database contract, section
9.4) and, once the checks of 8.2 that need no device pass, hands the worker
a `WriteOrder`: the request, the generation it was taken in, the output's
configuration, and the action the worker must take. Operations:

| Operation | What the driver does |
| --- | --- |
| `DIRECT` | Writes the value |
| `SELECT` | Selects the output for this request (8.4) |
| `OPERATE` | Writes the value of a live selection (8.4) |
| `CANCEL` | Ends a selection (8.4) |
| `READ_NOW` | Reads the target's point at once (8.12) |

### 8.2 Checks, in order

The checks run in this order. The first that fails answers the request and
stops it; nothing is sent to the device. Every check runs for every source,
AUTO included.

| # | Check | Thread | Fails with |
| --- | --- | --- | --- |
| 1 | The library's: not expired, lifetime within 60 s, the record valid (tag database contract, 9.2, 9.4) | Library | EXPIRED, or passed over |
| 2 | The target handle is a point of this driver with an output, or a readable point for `READ_NOW` | Command | REJECTED, `UNKNOWN_TARGET` |
| 3 | The operation is allowed for the output: `SELECT`, `OPERATE` and `CANCEL` only on an output configured `sbo`; `DIRECT` from OPERATOR only on an output configured `direct` (an operator command to an `sbo` output must select); `DIRECT` from AUTO or REMOTE on either | Command | REJECTED, `WRONG_CONTROL_MODEL` |
| 4 | Value: `value_type` is the tag's type; the value is within the output's configured limits (a union of intervals, a release value, an on-only command that has no off value) | Command | REJECTED, `INVALID_VALUE` |
| 5 | Source against mode (reference 8.1): AUTO only while the output's mode tag reads AUTO, OPERATOR only while it reads MANUAL, REMOTE only while it reads REMOTE. The mode tag is read at this check; a read that is not `SCADA_OK` with GOOD quality fails the check (fail closed) | Command | NOT_AUTHORISED |
| 6 | Selection rules (8.4) | Command | REJECTED, `NOT_SELECTED`, `ALREADY_SELECTED`, `SELECTION_MISMATCH` |
| 7 | Device state: `MISMATCH` or `DISABLED` | Command | REJECTED, `CONFIG_MISMATCH` |
| 8 | Write rate limit of the point (8.7) | Command | REJECTED, `RATE_LIMITED` |
| 9 | The order queue to the worker has room | Command | REJECTED, `BUSY` |
| 10 | Device connected and not `DEMOTED` or `REFUSING` at the moment the worker starts the order; a demoted device is attempted once with its timeout, because a command is an explicit intent | Worker | REJECTED, `COMM_FAIL` (nothing sent) |
| 11 | `expiry_mono_ns` checked again on the worker's clock right before the first byte goes to the device (tag database contract, 9.2) | Worker | EXPIRED (sent by the command thread, from the worker's outcome) |

- Decision D-92 records these checks.
- Check 5 is the driver's part of the output model (reference 8.1). The
  mode tag is owned by `scada-cmd`; while `scada-cmd` is down the tag reads
  BAD with OWNER_DOWN and every source is refused. In phase 2, before
  `scada-cmd` exists, tests own the mode tag. Mode values (INT64): 1 AUTO,
  2 MANUAL, 3 REMOTE; any other value fails the check. The coding is
  agreed with #98 (section 15, B-5).
- Checks 2 to 9 make no device I/O and no system call on the worker; they
  run on the command thread, which may read tags (`scada_tag_read` is safe
  from any thread, tag database contract section 13.2).
- `scada-cmd` checks value limits, interlocks, authority and mode itself
  (reference 8.2). The driver checks them again because AUTO and REMOTE
  requests do not pass through `scada-cmd` (reference 8.4), and because
  the driver is the last place before the device. The driver does not
  evaluate interlocks: they are in the logic for AUTO (reference 8.4) and
  in `scada-cmd` for OPERATOR.

### 8.3 Write encodings

An output's encoding is an ordered list of protocol writes per value, from
configuration (#98): fixed values, separate addresses for on and off,
inverted polarity, a mask written as a whole word (no read-modify-write),
exactly the units a device demands, a write type other than the read type.
The protocol compiles it at `configure` and refuses what it cannot send
(`DRV-CFG-07`).

- A list of several writes is sent in order, on one connection, with no
  other transaction to that device in between (an exclusive sequence,
  6.6). If a write of the list fails after an earlier one succeeded, the
  result is DEVICE_ERROR with reason `PARTIAL`, the remaining writes are
  not sent, and the framework reads back every member (8.8).
- The framework never reads a word, changes bits and writes it back, and
  never resends a stored image of a word. A whole-word command is written
  from the configured value and mask only.
- Writes of several tags in one request (a curve, a table) are not in
  phase 2 (survey C-47): one request carries one target (tag database
  contract, section 9.4). A curve is written point by point, or with the
  vendor's tool. Room: a later contract may add write groups as one output
  whose encoding writes many units from several request tags.

### 8.4 Select before operate

**open: needs the product owner's ruling (X-1 and X-7).** **Review by a
person.**

Reference 6.4: "Where a protocol has a native select-before-operate
procedure, the driver must use it for `SELECT` and `OPERATE` requests.
Where it does not (Modbus), the driver accepts `OPERATE` after a `SELECT`
held by the command gateway (section 8.2)." The survey found a relay whose
Modbus map has a device-side select, and protocols that have no select for
some points. This draft makes native select a property of the device and
point, declared by the protocol in its capabilities (X-1, option (b)), and
keeps the gateway's select for every point without it. Phase 2 implements
the gateway select; a native select over Modbus, where a device has one, is
room until the device's procedure is documented (annex A, M-12).

**Selections.** The driver keeps one selection table per output, owned by
the command thread. A selection is created by a SELECT that passed every
check and was executed, and holds: the SELECT's request id, the source, the
`user_id`, the value, the time it was made, and whether a native selection
is outstanding on the device.

**Rules:**

1. A SELECT on an output with a live selection is refused
   (`ALREADY_SELECTED`), whoever sends it (survey D-29 (6)). `scada-cmd`
   already refuses a second user (reference 8.2, step 6).
2. An OPERATE or CANCEL must name, in `select_request_id`, the request id
   of the live selection of its target; the same source, the same
   `user_id`; and, for OPERATE, the same value (survey D-29 (4)).
   Otherwise: `NOT_SELECTED` (no live selection with that id) or
   `SELECTION_MISMATCH`.
3. A selection ends at the first of: its OPERATE (whatever the result);
   its CANCEL; its timeout; the loss of the connection while a native
   selection is outstanding (counted in `sys.dev.<device>.selections_lost`);
   a reload of the output; a generation change; a driver restart (tag
   database contract, section 9.2). An OPERATE or CANCEL for an ended
   selection is `NOT_SELECTED`.
4. The driver times its selections itself (tag database contract, section
   17, item 19: a driver never sees an expired OPERATE or CANCEL). The
   selection timeout is the output's `select_timeout` (reference 8.2 step 6,
   default 30 s), at most 60 s, the request lifetime.
5. A CANCEL with no live selection is answered OK with reason
   `NOTHING_TO_CANCEL`; nothing is sent. A CANCEL of a live native
   selection, and the timeout of one, send the device's cancel when the
   protocol has one (survey D-29 (2)).
6. SELECT and OPERATE of one selection go over one connection, serialised
   by the worker. A selection whose connection was lost cannot be operated
   (rule 3).

**Without native select** (the gateway select; every point in phase 2): a
SELECT that passes the checks makes the selection and is answered OK; no
device I/O. Its OPERATE writes the value as a DIRECT would.

**With native select** the device has a window W, declared by the protocol
per point (`native_select_window`), and possibly a configured shorter one.
The device windows the survey found range from about 1 s to over 60 s. The
gateway's select must suit an operator (reference 8.2, step 6, default
30 s), which a 1 s window does not. The output chooses with `native_select:
at_select | at_operate` (#98, B-5); this draft proposes, decision D-89
(X-7):

- **`at_select`** (the default): SELECT makes the device's select at once;
  the device's own checks (its interlock, its synchrocheck) answer at
  select time, where the operator sees them; OPERATE makes the device's
  operate; CANCEL and the selection timeout make the device's cancel. The
  output's `select_timeout` must be at most W: otherwise the configuration
  is refused (`DRV-SBO-02`).
- **`at_operate`**: the gateway holds the operator's selection, and the
  driver runs the device's select and operate back to back at OPERATE. The
  sequence is fixed below.
- A `DIRECT` from AUTO or REMOTE on a point whose device requires a select
  (its control model has no direct operate) runs the `at_operate` sequence,
  steps 3 to 7.

The `at_operate` sequence:

1. **SELECT** (from `scada-cmd`, at reference 8.2 step 6). Checks 2 to 9 of
   8.2 run. The driver makes a gateway selection and answers OK. Nothing
   goes to the device: the SELECT verifies the request, the mode and that
   the output is not selected, and **nothing on the device** (not that it
   is reachable, not its local or remote state, not its interlocks). The
   operator's confirmation follows within `select_timeout`.
2. **OPERATE** arrives within `select_timeout`. Checks 2 to 9 run, with
   rule 2 of the selection rules.
3. The worker takes the order at the next scan boundary, before any due
   poll (6.2), and makes checks 10 and 11.
4. It sends the device's select. Deadline: the earlier of the request's
   expiry and the device's `timeout`.
5. On a positive select answer, at time `t_sel`, it checks the expiry
   again and sends the device's operate at once: no other transaction to
   that device, and no other order of the channel, between steps 4 and 6.
   If `t_sel + W` would pass before the operate can be sent (a slow answer
   that used up the window), it sends the device's cancel instead and
   answers REJECTED with `SELECT_EXPIRED`.
6. It waits for the operate's answer. Deadline: the earlier of the expiry
   and `write_timeout`.
7. The result:
   - select refused by the device: REJECTED, `SELECT_FAILED`, the
     device's cause as the raw code; nothing more is sent;
   - select unanswered at its deadline: the device's cancel is sent when
     the protocol has one; REJECTED, `SELECT_FAILED`, raw 0. No operate was
     sent, so the command was not executed;
   - operate refused: REJECTED with the reason the protocol maps (for
     example `REJECTED_INTERLOCK`);
   - operate unanswered: TIMEOUT, `NO_RESPONSE` (outcome unknown);
   - operate accepted: OK, or the two-phase rule of 8.6.

   In every case the selection ends (selection rule 3).

Timing: steps 3 to 7 take at most one scan wait, plus two transactions on
the device, plus one scheduling step between them. On a channel with a
device timeout of 1 s that is about 2 s worst case after the OPERATE was
taken, within the expiry `scada-cmd` sets (reference 8.2, step 9). A device
whose window W is shorter than the time between two of its transactions
cannot be operated this way. Its outputs are configured `direct`, or the
device's window is raised; `DRV-SBO-01` refuses an `at_operate` output
whose W is below twice the device's `timeout`.

Reason: `at_select` keeps the device's checks at select time whenever the
device's window allows it; `at_operate` keeps the operator's 30 s with a
device whose window is too short for a person, and still uses the device's
select procedure, as reference 6.4 requires. The cost of `at_operate` is
that a refusal by the device comes at OPERATE, after the operator
confirmed; the operator then sees a failed command with the device's
cause.

### 8.5 Expiry

- The library answers EXPIRED what has expired before the driver takes it
  (tag database contract, section 9.2).
- The worker checks `expiry_mono_ns` again right before it acts on the
  device (check 11) and before each write of a list (8.3). A request that
  expires between two writes of a list stops there: DEVICE_ERROR with
  reason `PARTIAL` when something was sent, EXPIRED when nothing was.
- A transaction's deadline is never later than the request's expiry. A
  write whose answer has not arrived by the expiry is answered TIMEOUT
  (outcome unknown).
- A request is never executed after its expiry, never twice, and never
  after the driver detached from the generation it was taken in (section
  9).

### 8.6 Results

One result per request taken, posted by the command thread (tag database
contract, section 9.2), at the request's final outcome. Status values are
the seven of the tag database (frozen). The driver adds a reason and the
raw protocol code in `WriteResult.protocol_error` (decision D-85):

```
protocol_error = (reason << 16) | raw        // reason 0 .. 32767, raw 0 .. 65535
```

- `raw` is the protocol's own code (a Modbus exception code, a protocol's
  cause), or 0. A raw code wider than 16 bits is given as 0xFFFF here, and
  in full in `sys.dev.<device>.last_status`.
- `reason` is from the table below; 0 is "no reason".

What the field means today: tag database contract 1.0, section 9.4,
`int32_t protocol_error; // driver-specific, 0 if none`; reference 5.3
lists "protocol error code" among the result's fields. The library
checks only the reserved fields and `status` of a result (tag database
contract, section 14.1), and no consumer reads `protocol_error` before
phase 5. The packing keeps the field driver-specific for the library and
keeps 0 when there is nothing to say; it only fixes, for every driver, what
"driver-specific" holds, so `scada-cmd` reads one form. This draft holds
that this changes no meaning of the tag database contract: no reader of
1.0 is affected and no layout version is needed. If the owner reads
"protocol error code" in reference 5.3 as the raw code alone, the packing
is a change to the result's meaning, and the alternative is the raw code in
`protocol_error` and the reason in a device diagnostic tag. **open: needs
the product owner's ruling (R-4).**
- The mapping from protocol codes to reasons is the protocol's, and may be
  overridden per device type in configuration (a device that uses a
  protocol's code with another meaning).

| Reason | Value | Status | Meaning | May the requester retry |
| --- | --- | --- | --- | --- |
| (none) | 0 | OK | Accepted by the device | |
| `ALREADY_IN_POSITION` | 1 | OK | The device reports nothing to do | |
| `NOTHING_TO_CANCEL` | 2 | OK | CANCEL with no live selection | |
| `UNKNOWN_TARGET` | 10 | REJECTED | Not an output of this driver | No |
| `WRONG_CONTROL_MODEL` | 11 | REJECTED | Operation not allowed for the output, or the device refused it for its control model | No |
| `INVALID_VALUE` | 12 | REJECTED | Type or limits, or the device refused the value | No |
| `NOT_SELECTED` | 13 | REJECTED | No live selection with that id | No; select again |
| `ALREADY_SELECTED` | 14 | REJECTED | A live selection exists | No |
| `SELECTION_MISMATCH` | 15 | REJECTED | Source, user or value differ from the SELECT | No |
| `SELECT_FAILED` | 16 | REJECTED | The device refused its select | No |
| `SELECT_EXPIRED` | 26 | REJECTED | The device's window passed before the operate could be sent (8.4, `at_operate` step 5); cancel sent | No; give the command again |
| `CONFIG_MISMATCH` | 17 | REJECTED | Device in `MISMATCH` or disabled | No |
| `RATE_LIMITED` | 18 | REJECTED | The point's write rate limit (8.7) | Later |
| `BUSY` | 19 | REJECTED | The driver's order queue was full, or the device answered busy and did not execute | Yes |
| `REJECTED_INTERLOCK` | 20 | REJECTED | The device's interlock refused | No |
| `REJECTED_SYNCHROCHECK` | 21 | REJECTED | The device's synchrocheck refused | No |
| `REJECTED_LOCAL` | 22 | REJECTED | The device is in local control | No |
| `REJECTED_MODE` | 23 | REJECTED | The device's own mode or a missing enable refused (a permanent state refusal) | No |
| `NOT_SUPPORTED` | 24 | REJECTED | The device does not have the function | No |
| `COMM_FAIL` | 25 | REJECTED | Not connected; nothing was sent | Yes |
| `PARTIAL` | 30 | DEVICE_ERROR | Part of a list of writes was applied | Read the feedback first |
| `DEVICE_FAULT` | 31 | DEVICE_ERROR | Any other error answer | No |
| `NO_RESPONSE` | 40 | TIMEOUT | Sent, no answer: whether it was executed is unknown | Read the feedback first |
| `TIMEOUT_CONFIRM` | 41 | TIMEOUT | No confirmation of a two-phase write | Read the feedback first |
| `TIMEOUT_TERMINATION` | 42 | TIMEOUT | Confirmed, but no termination in time | Read the feedback first |
| `EXPIRED_BEFORE_SEND` | 50 | EXPIRED | Check 11 | No |
| `NOT_STARTED` | 60 | ABORTED | Taken, not started, and the generation retired or the driver stopped (section 9) | No |
| (any) | | NOT_AUTHORISED | Check 5; reason 0 | No |

- "May the requester retry" is guidance for `scada-cmd` and `scada-control`;
  the driver itself never retries a write (8.7).
- **Two-phase writes** (room; survey D-30): a protocol whose write is
  confirmed and later terminated (`write_completion: two_phase`) calls
  `WriteCompletion::accepted()` and later `completed(outcome)`. The one
  result is posted at completion. The termination must arrive within the
  output's `termination_timeout` (default: the request's expiry), otherwise
  TIMEOUT with `TIMEOUT_TERMINATION`. A negative confirmation completes at
  once with its refusal.

  Checked against the command gateway (reference 8.2). After `scada-cmd`
  posts the request (step 9) its status tag reads `EXECUTING` (reference
  8.1) until the result arrives; then it waits for the feedback within
  `feedback_timeout` (step 10). With one result at completion:
  - a refusal at confirmation (local mode, interlock, wrong control model)
    reaches `scada-cmd` as fast as with a one-phase write;
  - a long run (a breaker travelling, a termination tens of seconds later)
    shows as `EXECUTING` for the whole run. That is what an operator needs
    to see: the command is with the device and has not failed. The
    feedback tag changes during the run and is on the operator's screen
    meanwhile;
  - the time limits are the request's expiry, set by `scada-cmd` (at most
    60 s, tag database contract 14), and the output's
    `termination_timeout` within it; `feedback_timeout` starts after the
    result, when the device says it has finished.

  So `scada-cmd` need not learn "accepted" to do its steps. It could not
  learn it from a second result anyway: a requester drops a second result
  for an id it already holds (tag database contract, section 9.2, "the
  consumer ignores an id it has already stored"), so two results per
  request would be a change to that contract. The stage is published for
  people in `sys.dev.<device>.last_command_stage` (INT64: 1 sent, 2 accepted, 3
  completed) with its time. If the owner wants the stage per command on the
  HMI, the room is a driver-owned stage tag per output (INT64), with no
  layout change; this draft does not add it.
- Static protocol options of a command (a qualifier, check bits, an
  originator, a pulse qualifier, a protocol's control code per state) come
  from the output's configuration, looked up by the target. The frozen
  `WriteRequest` has no room for them and needs none: they do not change
  per request. A test flag per request is not supported.

### 8.7 No retry, rate limits

**Review by a person.**

- The framework never resends a write once any byte of it was sent. A
  write that timed out is TIMEOUT with `NO_RESPONSE` (outcome unknown;
  survey D-36). A trigger is sent once. This holds for every source
  (reference 14.4: "no automatic retry of operator commands"; for AUTO the
  next cycle of `scada-control` decides).
- A device's "busy, not executed" is answered REJECTED with `BUSY`; the
  requester decides. Room: a protocol may declare a write `idempotent` and
  a busy answer definite, and configuration may then allow `busy_retries`
  (default 0) within the expiry. Phase 2 has none.
- Write rate limits per point (survey C-48, D-35): `min_interval` and, for
  `storage: nvm`, `max_per_day`. A request that would exceed either is
  refused (`RATE_LIMITED`) whatever its source; nothing bypasses it but a
  reload with a new limit. An `nvm` output cannot have a cyclic source
  (`DRV-NVM-01`). Phase 2: the counts are kept in memory, so a driver
  restart starts a new day count; durable counts are room (with D-26 of the
  survey, per-device durable state).

### 8.8 Accepted is not done

- OK means the device accepted the write by its protocol, and nothing more
  (survey D-32): devices accept writes they ignore (a command bit that
  acts only through site logic, a value outside a range the device
  silently discards, a reset ignored in a locked mode). Whether the plant
  followed is decided by `scada-cmd` from the feedback tag within the
  output's `feedback_timeout` (reference 8.2, step 10). The driver never
  reports a command as having reached its feedback.
- Read-back: after an OK, PARTIAL or TIMEOUT write the framework schedules
  a `READ_NOW` of the output's feedback or read-back point after its
  `settle` time (default 0: at the next scan boundary), and after an edge
  command (a pulse, a trigger), so that the feedback tag is fresh when
  `scada-cmd` compares it. The read-back is not part of the result.
- An output without read-back (`no_feedback: true`, reference 12.5) whose
  point has a tag: the framework writes the value last written to it with
  UNCERTAIN and LOCAL_TIME, the source time the time of the OK. UNCERTAIN
  is the mark "written, not confirmed"; no new flag is needed. A `wo` point
  is never polled.

### 8.9 Other masters

Another master may command the same device. Its commands show only as
changes of the feedback tag, which the driver reports like any change. The
driver keeps no image of the device's outputs beyond its selections, and
takes nothing it wrote as still true.

### 8.10 Refresh of setpoints

**open: needs the product owner's ruling (X-5).** **Review by a person.**
Phase 5; room in phase 2.

Some devices drop a setpoint after a validity time, or fall back when
setpoints stop arriving. Such outputs need the value written again
cyclically without a new operator action. "No audit, no action" (reference
5.3, 8.2 step 8; tag database contract 9.3) must hold. This draft proposes
option (a), decision D-90:

- An output configured `refresh: <period>` is written again by the
  framework with the value of the last request executed with OK, every
  period, as part of that request: its event record covers the refreshes,
  and no new record is posted for them. The record must say that the
  output refreshes (the journal schema of phase 5 carries it).
- A refresh is a write of the same value to the same target from the same
  source; it is made only while all of these hold, checked before each
  refresh:
  - the output's mode tag reads the mode of that request's source, GOOD
    (as check 5);
  - the request tag of that mode (reference 8.1: the auto or the manual
    request tag) reads GOOD, not STALE, not OWNER_DOWN, and holds the
    refreshed value;
  - the generation is the one the request was taken in, and the driver
    has not restarted;
  - the device is connected and not `MISMATCH`.
- When any condition fails, refreshing stops for good; it starts again only
  with a new request. The device's own fallback then acts (reference 14.4:
  the device's communication-loss behaviour is the barrier when the unit
  is gone).
- Refreshes obey the rate limits (8.7) and the pacing (6.4), and are never
  retried; a failed refresh is counted (`sys.dev.<device>.refresh_failures`)
  and the next period tries again.
- A refresh is not a command: it never selects, and it is not offered to
  outputs configured `sbo`.

For AUTO outputs `scada-control` already posts again "whose refresh
interval expired" (reference 7.2, step 3); an AUTO output may use either,
not both (`DRV-CFG-09`, to be added with phase 5).

### 8.11 Shutdown and loss of the source

**open: needs the product owner's ruling (X-9).** **Review by a person.**
Phase 5 for protocol drivers; phase 2 for the local outputs of `scada-io`
(#107).

Reference 14.4: on orderly shutdown outputs follow `on_shutdown: hold |
safe`; when `scada-control` stops, `scada-io` and drivers apply each
affected output's configured behaviour, hold or safe after a timeout.
Reference 6.5 gives `scada-io` its safe states. For a remote device "safe"
means writing a value: a safe value, or a release value that hands control
back to the device's own settings. This draft proposes, decision D-91:

- The framework can do both (survey D-34, X-9 option (b)): stop refreshing
  only (`hold`), or write once the output's configured release value
  (`release`, the output's `limits.release`) or safe value (`safe`, the
  output's `safe_state`). The keys are `on_shutdown` and `on_source_loss`
  (configuration schemas contract, 7.2), each `hold | safe | release`;
  `hold` and `safe` are the words of reference 14.4.
- When: `on_shutdown` at an orderly stop of the driver, for each connected
  device, before disconnect; `on_source_loss` for an output in AUTO mode,
  when the process that owns its auto request tag (`scada-control`) has
  been down for the output's `source_lost_after` (reference 14.4, "after a
  timeout").
- "No audit, no action" holds for these writes too: before a safe or
  release write the driver posts its own event record to its event queue
  (`events.drv-<protocol>`, a queue the tag set gives from phase 5 to every
  driver with an output whose `on_shutdown` or `on_source_loss` is not
  `hold`; section 15, B-8). If the post is refused, the driver
  does not write: it stops refreshing (as `hold`) and relies on the
  device's own fallback, and counts the case
  (`sys.proc.<process>.safe_writes_unjournalled`). Reason: the device's fallback
  is the barrier the reference names for a unit that cannot act (14.4); a
  fail-safe write without its record would be the first action without
  audit.
- The safe write is a DIRECT write, never retried, bounded by the stop
  time of the service manager; its outcome is logged.
- After a driver restart or a generation change, no output is written
  until a new request arrives (reference 14.4: outputs are not driven until
  the control runtime has run a cycle).

`scada-io` in phase 2 (#107) applies `set_safe_state` of its HAL to local
outputs on stop and on the loss of `scada-control`'s heartbeat (reference
6.5), journalled the same way once the events consumer exists (phase 5);
until then its tests check the hardware state, and the journal half is
phase 5.

### 8.12 `READ_NOW`

`READ_NOW` reads the target's point outside its poll group, at the head of
the channel's order queue. It changes nothing on the device: it is refused
(REJECTED, `NOT_SUPPORTED`) for a `side_effect` or `wo` point. It is
answered OK when the read returned outcome `VALUE`, `NO_VALUE`, `ABSENT` or
`DEVICE_INVALID` (the device answered; the tag says how), and TIMEOUT when
it was `COMM_FAIL`. It needs no mode check and no selection; it is still a
request and needs its event record (tag database contract 9.3). Phase 2.

## 9. Generation change and restart

This section answers #84. **Review by a person** (restart behaviour).

### 9.1 After every attach

- The framework writes every tag of each of its owners before the
  worker's first scan, on the worker's thread (tag database contract,
  section 6.3: a reused record may read GOOD with an old value): value as
  stored, quality BAD, flag NOT_INIT, `source_time_ns` 0 ("no device
  time"), except a tag restored from a retain file (RESTORED), which keeps
  its restored value until the device is read. Diagnostic tags are written
  with their current values. Phase 2.
- Handles are resolved again; the plan is kept unless the configuration
  changed (section 11).
- No output is written by the driver until a request of this generation
  asks for it (8.11).

### 9.2 Duties in a RETIRING generation

When the main thread learns that the generation is RETIRING
(`scada_tag_heartbeat` or `scada_tag_status` returns `SCADA_E_RETIRING`):

1. **New requests.** The command thread calls `scada_queue_next` on every
   `cmd` queue until it returns `SCADA_E_RETIRING`; the library answers
   ABORTED what is still queued (tag database contract, section 9.5). The
   command thread dispatches nothing new.
2. **Requests taken and not started.** An order in a worker's order queue
   that the worker has not started is returned unstarted; the command
   thread answers it ABORTED with reason `NOT_STARTED`. It was not executed,
   so ABORTED is true, and results are accepted in a RETIRING generation
   (tag database contract, 9.5).
3. **Requests in flight.** An order the worker has started completes, and
   its answer is posted, before the process detaches. Its bound is its
   deadline (at most its expiry). A two-phase order whose termination has
   not arrived when the drain budget of the process (1 s, tag database
   contract, section 11.3) ends is answered TIMEOUT with
   `TIMEOUT_TERMINATION`, and a termination arriving later is ignored. The
   driver never acts on a request after it detached: otherwise the
   operator gives the command again in the new generation and the output
   operates twice.
4. **Selections** end (8.4, rule 3). A live native selection gets the
   device's cancel, when the device is connected and the protocol has one,
   before detach.
5. **Refreshes** stop (8.10): they belong to requests of the old
   generation.
6. **Polling** goes on, and the workers write tags until the main thread
   pauses them at a scan boundary to detach (tag writes stay allowed until
   detach, tag database contract 11.3).
7. **Detach** runs on the main thread, outside every cycle: a real-time
   worker never calls `scada_queue_next` and is paused, not detached, so no
   system call of the RETIRING path (tag database contract, 9.5) runs on a
   real-time thread.
8. **No answer across the change.** Every outcome a worker returns carries
   the generation of its order. After the reattach the command thread
   drops, logs and counts (`sys.proc.<process>.stale_outcomes`) any outcome of
   an older generation; it never posts it: ids restart at 0 and could name
   a request of the new generation.
9. **Reattach:** attach to `current`, resolve, the writes of 9.1, resume the
   workers. Connections stay open across the change unless the reload
   changed the channel (section 11): device sessions do not belong to a
   generation.
10. **Outputs** hold, or go to their safe behaviour, until they are written
    again in the new generation: AUTO values by `scada-control`, operator
    commands by `scada-cmd` (reference 5.5; #84). The driver writes
    nothing to them meanwhile, except the configured safe behaviour of
    8.11 when `scada-conf` asks for it (phase 5).

Steps 1 to 8 fit in the 1 s drain of the tag database contract, section
11.3, when every write deadline is at most 1 s; a longer deadline delays
the detach by its excess, and `scada-conf` then applies the limit of
reference 5.5. Phase 2.

### 9.3 Driver restart

A driver that crashed or stopped has lost every selection and every order
it held; the library answers ABORTED what is still queued at its next
attach (tag database contract, section 9.2). A request it took and did not
answer is reported by its requester as failed with its outcome unknown
(tag database contract, section 9.5). After the restart, 9.1 applies.

## 10. Diagnostics

Every driver publishes these system tags (reference 6.3, survey D-39). The
framework writes them; a protocol only reports counts and states. Owners:
channel and device tags are written by the channel's worker; process tags
by the main thread. Counters are UINT64 since the process attached, and
start at 0 at every attach. Times are INT64 nanoseconds UTC. Names are
agreed with #98 (section 15, B-8).

Per process, `sys.proc.<process>.<signal>`:

| Signal | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `realtime` | BOOL | | The real-time tier is in use (reference 4.3) |
| `stale_outcomes` | UINT64 | count | Outcomes of an older generation dropped (9.2) |
| `answers_dropped` | UINT64 | count | Answers the command thread could not post |
| `safe_writes_unjournalled` | UINT64 | count | Safe writes not made for lack of a journal record (8.11) |
| `config_errors` | UINT64 | count | Errors at the last configure or reload |

Per channel, `sys.drv.<channel>.<signal>`:

| Signal | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `state` | INT64 | | 0 disconnected, 1 connecting, 2 connected |
| `connects` | UINT64 | count | Successful connects |
| `period.<group>` | FLOAT64 | ms | Achieved period of each poll group, averaged over its last 10 scans |
| `requests_per_s` | FLOAT64 | 1/s | Requests sent, over the last 10 s |
| `deadline_overruns` | UINT64 | count | Protocol calls that passed their deadline (2.3) |
| `scan.<group>` | UINT64 | count | Scans of a synchronous scan group (6.7) |
| `samples_dropped` | UINT64 | count | Samples a helper thread could not hand over (2.6) |

Per device, `sys.dev.<device>.<signal>`:

| Signal | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `state` | INT64 | | Device state of 3.4 |
| `last_ok_time` | INT64 | ns UTC | Last successful transaction |
| `requests` | UINT64 | count | Transactions sent |
| `errors` | UINT64 | count | Transactions with an error answer |
| `timeouts` | UINT64 | count | Transactions without an answer |
| `response_ms` | FLOAT64 | ms | Average response time over the last 100 transactions |
| `requests_per_scan` | UINT64 | count | Requests of the plan, all groups (6.3) |
| `pacing_limited` | BOOL | | A group of the device misses its period because of pacing (6.4) |
| `last_status` | INT64 | | Last protocol error or exception code, raw |
| `exceptions` | UINT64 | count | Error answers with a protocol code (per code: in the debug log) |
| `refusals` | UINT64 | count | Answers that refused, apart from timeouts (busy, gateway, not started) |
| `sentinels` | UINT64 | count | `NO_VALUE` samples (5.2) |
| `invalid` | UINT64 | count | `DEVICE_INVALID` samples |
| `points_absent` | UINT64 | count | Points currently `ABSENT` |
| `identity` | STRING | | Identity read (3.5) |
| `identity_ok` | BOOL | | Identity matched |
| `restarts` | UINT64 | count | Device restarts detected (7.5) |
| `selections_lost` | UINT64 | count | Native selections lost on disconnect (8.4) |
| `last_command_stage` | INT64 | | 8.6 |
| `refresh_failures` | UINT64 | count | 8.10 (phase 5) |
| `events_lost` | UINT64 | count | 5.7 (room) |

The device's "connection state, last successful poll time, actual poll
period per group, request count, error count, timeout count and average
response time" of reference 6.3 are `state`, `last_ok_time`,
`period.<group>` (per channel), `requests`, `errors`, `timeouts` and
`response_ms`. Room for phase 9: interrogation time, replay counts, key and
certificate age, last time sync.

## 11. Reload

A configuration commit reaches the driver as a reload signal for changes
that keep the tag set, or as a generation change for changes that alter it
(reference 5.5, 12.4).

| Change | Applied | How |
| --- | --- | --- |
| Poll group period or priority; device timeout, retries, back-off, pacing, keep-alive; scaling; invalid markers; `max_age`; read rules | Live | Worker paused at a scan boundary; `configure` and `plan` for the devices concerned; selections kept |
| A device's addresses, encoding or identity rules (a changed site map that keeps the tag set; survey D-27) | Live, that device only | The device goes `INITIALISING`; its points NOT_INIT until read; its selections end |
| An output's encoding, limits, select model, select timeout, refresh, rate limits, shutdown behaviour | Live, that output only | Its selection ends; refreshing stops until a new request (8.10) |
| A channel's port, host, serial settings, endpoints | Live, that channel only | Its devices disconnect and reconnect; their selections end |
| A device or channel enabled or disabled | Live, that device or channel only | Disabled: its selections end, it disconnects, its points are written BAD with NOT_INIT and `state` 0. Enabled: it goes `CONNECTING`. Its tags exist either way |
| Points, devices or channels added or removed; a tag type changed | Generation change | Section 9 |
| The protocol's own code, the framework's version | Process restart | 9.3 |

`configure` refusing a reload leaves the running configuration in force and
reports the errors (`sys.proc.<process>.config_errors`). `scada-conf` validated
the tree before the commit, so this is a fault of one of the two validators
(section 4) and is logged at ERROR. Live reload completes within 2 s
(reference 15.2). Phase 2.
## 12. Simulator requirements

Every driver ships with a simulator of its protocol (`CLAUDE.md`, "Tests").
A simulator is a separate process that serves configurable simulated
devices over the real transport (a TCP port, a pseudo-terminal pair), so
that integration tests start real driver processes against it. It is
controlled by a test through a control socket or file, never by the driver
under test.

Every simulator, whatever its protocol, must be able to:

1. Serve many devices: at least 300 across its instances, on one or many
   connections (reference 18.2, phase 2; #108), with a configured point map
   per device and values that follow a pattern or are set by the test.
2. Answer late by a set delay, not answer, answer with each of the
   protocol's error answers, close the connection, refuse a connection, and
   stay silent after a "power-up" for a set time.
3. Enforce pacing: a minimum interval between requests, and a limit of
   values per request, refusing or delaying what breaks it.
4. Close an idle connection after a set time, and limit its connections
   (refusing the next).
5. Serve gaps in a map as the protocol's "illegal address", as zeros, or as
   typed sentinels; refuse a read that splits a multi-unit value.
6. Treat a point as a side-effect read (clear on read, per client), and as
   write-only.
7. For writes: accept and apply; accept and ignore; accept and clamp;
   refuse with a chosen error; answer busy; leave unanswered; apply a part
   of a list; apply after a delay visible in a read-back elsewhere. Count
   the writes per point.
8. Offer a select procedure where the protocol has one, with a window, and
   a direct operate where it has one.
9. Change a device's identity, and signal a device restart.
10. Report to the test every request it received, in order, with its time,
    so that tests count requests (block merging, pacing, no retry of
    writes, writes first).

Phase 2 (#105, #106): the Modbus TCP and RTU simulators provide 1 to 10
with the Modbus specifics of annex A (exception codes, unit-id routing with
a gateway's offline and cached states, function refusal per device). The
`scada-io` simulator backend (#107) provides 1, 2 (silence only), 6 and 7
(apply and ignore) for its HAL calls, and reports its outputs' states and
safe states. The survey's six invented test profiles are inputs to the
composition of the 300-device test (#108), not part of this contract.

## 13. Conformance tests

These tests are protocol-independent. `libscada-driver` passes them with a
test protocol (an in-process `ProtocolDriver` with a scripted device,
#104); each phase 2 driver passes them against its simulator (#105, #106,
#107), with the protocol-specific tests of its own issue. Every test runs on
x86-64 and ARM64 and under the sanitizer builds; tests wait on conditions
with timeouts, never on sleeps.

Interface and threads

1. Every call of 2.3 runs on its thread: a debug check in the test protocol
   stops the process on a call from another thread.
2. A protocol call that passes three times its deadline stops the
   heartbeat: the process reads down within its timeout (2.5).
3. A fast channel's scan parts 1, 3 and 4 allocate nothing: a counting
   allocator in the test protocol's process sees no allocation in 10,000
   scans; the `SCADA_RT` check passes on them.

Samples and quality

4. Each outcome of 5.2 gives the quality and flags of its row, and only
   `COMM_FAIL` counts toward demotion.
5. Several samples of one point in one delivery give that many change
   entries, in order.
6. Invalid markers are compared on the raw pattern before decoding (a NaN
   marker matches any NaN; an exact marker only its pattern).
7. A polled sample carries LOCAL_TIME and the receive time; a sample with a
   device time carries it, converted from a configured local zone.
8. A dependent point with a failed companion is BAD; a plan never
   separates them.

Scheduling

9. Pending orders are served before due poll groups; groups by priority,
   then by due time.
10. Block merging: a poll group of N adjacent points within the rules sends
    the planned number of requests (counted by the scripted device); a
    forbidden range, an atomic group and a `side_effect` point are honoured.
11. A merged request refused as "illegal address" is split once; the points
    refused again are `ABSENT`, the others GOOD, and stay so until
    reconnect.
12. Pacing: no two requests to a device closer than its interval; a slow
    device does not delay a neighbour's group beyond one transaction.
13. Keep-alive: a device idle for its keep-alive period gets a request.
14. A synchronous scan group increments its counter once per scan before
    the notify.

Failure handling

15. A device that stops answering is demoted after `demote_after` failures,
    its tags BAD with COMM_FAIL, probed at the back-off, and recovered when
    it answers; timeouts within `startup_grace` do not count.
16. A lost connection sets every device of the channel BAD with COMM_FAIL at
    once, without spending retries.
17. A point not read within `max_age` gets STALE, and loses it at the next
    good read.
18. An identity mismatch with `refuse` sets `MISMATCH`, tags BAD without
    COMM_FAIL, and every write `CONFIG_MISMATCH`.

Write requests

19. Each check of 8.2 has a test that fails a request with its status and
    reason, and the device sees nothing.
20. Source against mode: AUTO in MANUAL, OPERATOR in AUTO, and any source
    with the mode tag BAD, OWNER_DOWN or unresolvable, give NOT_AUTHORISED.
21. Selections: a second SELECT, an OPERATE with a wrong id, source, user or
    value, an OPERATE after the timeout, after a CANCEL and after a
    reconnect, each refused with its reason; a CANCEL without selection is
    OK with `NOTHING_TO_CANCEL`.
22. Expiry: a request whose expiry passes while queued in the worker is
    answered EXPIRED and nothing is sent; a list of writes interrupted by
    expiry is `PARTIAL`.
23. No retry: a write that times out is sent once and answered TIMEOUT with
    `NO_RESPONSE`.
24. A rate-limited point refuses the request over its limit, from every
    source.
25. A request taken by a driver that is killed before it answers is never
    executed twice (tag database conformance test 17, with a driver; #104).
26. After an OK write the read-back point is read within its settle time
    plus one scan.

Generation change and restart (#84)

27. A generation change with a request queued, one taken and not started,
    and one in flight: the queued one is ABORTED by the library, the
    unstarted one ABORTED with `NOT_STARTED`, the one in flight executed
    once and answered before detach; none is acted on after detach; no
    answer is posted in the new generation; a selection of the old
    generation cannot be operated in the new one.
28. After every attach, every tag of the driver is written BAD with
    NOT_INIT before the first scan, except restored ones.
29. A real-time worker, after its first scan, makes only the system calls of
    the table of 2.4, across polls, writes and a generation change: no
    `scada_queue_next` and no call of the RETIRING path (traced with a
    seccomp or ptrace test helper on the worker thread).

Diagnostics and reload

30. Every tag of section 10 exists with its type and changes as its row
    says.
31. Each row of the reload table of section 11 applies as stated; a reload
    that `configure` refuses leaves the old configuration running.

Phase 2 drivers pass tests 1 to 31, except where the test protocol is the
only one that can force a case (2, 3 on a non-fast channel, 29 when the
driver has no fast channel); those then run with the test protocol only.
Tests for room parts (two-phase writes, reported points, replay, refresh,
safe writes of protocol drivers) are added with the phase that implements
them.

## 14. What phase 2 must implement

| Part | Section | `libscada-driver` (#104) | Modbus (#105, #106) | `scada-io` (#107) |
| --- | --- | --- | --- | --- |
| `configure`, `capabilities`, `plan`, `connect`, `disconnect`, `poll`, `write` (immediate), `read_now` | 2.3 | Calls them | Implements | Implements |
| Threads, order and outcome queues, channel watchdog | 2.2, 2.5 | Must | Inherits | Inherits |
| Real-time scan parts | 2.4 | Must | Real-time capable | Real-time capable |
| Event loop for report-driven protocols; helper threads | 2.6 | Loop without protocol descriptors | No | No |
| `resolve`, `session_step`, `run_activity`, `on_event` | 2.3 | Defaults only | No | No |
| `identify` and the identity check | 3.5 | Must | Configured points only | No |
| Device states 0 to 5 | 3.4 | Must | Inherits | Inherits |
| Several devices per channel, shared unit ids, unit id per point | 3.3 | Must | Must | n/a |
| Endpoint lists, listening channels | 3.3 | Room | No | No |
| Outcomes, invalid markers, tag types, companions, health, bits, latch tags | 5.2 to 5.6 | Must | Must | Outcomes and types only |
| Time of a sample | 5.8 | Must (LOCAL_TIME, device time with zone) | Must | Must |
| Replay, cause, events lost | 5.7, 5.9 | Room | No | No |
| Poll groups, write-before-read, block planning, pacing, keep-alive, side effects, scan groups | 6 | Must | Must | Poll groups and scan groups |
| Activities, record readers, device time sync | 6.1, 6.6, 6.8 | Room | No | No |
| Timeouts, connection loss, demotion, STALE, device restart | 7 | Must | Must | STALE, HAL failure |
| Checks of 8.2, gateway select, expiry, results and reasons, no retry, rate limits, read-back | 8 | Must | Encodes and classifies | Writes outputs |
| Native select, two-phase results | 8.4, 8.6 | Room | No | No |
| Refresh | 8.10 | Room (phase 5) | No | No |
| Safe and release writes | 8.11 | Room (phase 5) | No | Local safe states (#107) |
| Generation change and restart | 9 | Must | Inherits | Inherits |
| Diagnostics | 10 | Must, but the rows marked phase 5 or room | Reports | Reports |
| Reload | 11 | Must | `configure` per device | `configure` per channel |
| Simulators | 12 | Test protocol | Must | Simulator backend |

## 15. Boundaries with the configuration schemas contract

The two contracts were drafted in parallel (#98, #99). They meet at the
points below; each states who owns what. Sections of the configuration
schemas contract cited: 4.1 (common part and protocol part, X-8), 4.6
(channel and device keys), 4.7 (point keys), 4.8 (identify), 7.2 (outputs,
`native_select`), 12.3 (owners and queues), 12.5 (generated tags), section 10
(error codes), annex A (Modbus). The agreement with the #98 draft
is recorded in the pull request of this contract.

| Id | Point | Owned by #98 | Owned here |
| --- | --- | --- | --- |
| B-1 | Point address | The common keys; the merge of each protocol's schema fragments (channel, device, `address`, `read:`) into the file schemas; the Modbus fragments for phase 2 (its annex A) | That `configure` validates the binding again; the codes of 4.3; the raw sample and the processing after decode (5.1), which is the framework's |
| B-2 | Template per-protocol parts (X-8) | Option (b) now: one `protocol:` per template, `extends:` a protocol-free base; (a) room | That the protocol sees only its own binding |
| B-3 | Acquisition | `poll_groups`, `max_age`, `acquisition: poll \| report` (`poll` only in phase 2), report fields as protocol keys (room) | STALE (7.4), scheduling (6) |
| B-4 | Read rules | `read:` as a protocol key of template and device; `access`, `side_effect` | Their meaning and the planning (6.3, 6.6) |
| B-5 | Outputs and commands | Output keys: `sbo`, `select_timeout`, `native_select: at_select \| at_operate`, `termination_timeout`, limits, `refresh`, `write_on_change_only`, `on_shutdown` and `on_source_loss` (`hold \| safe \| release`), `source_lost_after`, safe and release values, `dangerous`, `storage` with `min_interval` and `max_per_day`, read-back with tolerance and `settle`, preconditions; the write encoding inside the point's `address`; the mode tag's coding (1 AUTO, 2 MANUAL, 3 REMOTE) | Checks and behaviour (8); results and reasons (8.6); double-point coding (5.4) |
| B-6 | Identity | `identify:` envelope, `on_mismatch` | Comparison, tags and the `MISMATCH` state (3.5) |
| B-7 | Sessions and secrets | `session:` reserved and refused in phase 2; a secret only as `{ secret: <name> }`, refused in phase 2 with its own code | Room for `session_step`; secrets never logged or in tags |
| B-8 | Tag set | Process `drv-<protocol>`; owners `drv-<protocol>` (main thread) and `drv-<protocol>.<C>`; generated short names `<C>` and `<T>` (its 12.3); queues `cmd.<C>.<source>` and `result.<C>.<source>` (`<source>` `cmd` or `<T>`), and from phase 5 `events.drv-<protocol>`; a disabled device keeps its tags (its 12.5); the diagnostic tags of section 10 by rule; ring sizes | Which thread writes which tag (2.2, 10) |
| B-9 | Channels | Channel `type` and framing as protocol keys (`tcp`, `rtu`, `rtu_over_tcp` in phase 2); `role: connect \| listen` with `connect` only; endpoint lists as room | 3.3 |
| B-10 | Addresses on a channel (X-4) | Overlap checked on (unit id, table, register); shared unit ids; `unit_id` per point address | 3.3 |
| B-11 | Timing and pacing | `timeout`, `retries`, `demote_after`, `backoff`, `startup_grace`, `min_request_interval`, `min_write_interval`, `keepalive`, `write_timeout`, `max_values_per_request`; `device_time`; `time_sync` reserved, `none` only | Their behaviour (6, 7) |

Secrets (B-7): **review by a person.** A secret (a password, a login code,
a key) is referenced by name in configuration and fetched by the framework
from the secrets store (reference 13.3) at connect. The framework passes it
to `session_step` in a buffer it zeroes after the call; it is never logged,
never written to a tag, never in a diagnostic. Session steps with secrets
are not in phase 2 (no secrets store exists before phase 4, and the steps
need their own review); the interface leaves room.

## 16. Open rulings and differences from the reference document

### 16.1 Open rulings

Each is drafted with the option shown; a person decides. Numbers follow the
device survey (X-n) or this contract (R-n).

| Id | Section | Question | Drafted with | Other options |
| --- | --- | --- | --- | --- |
| X-1 | 8.4 | Native select on a protocol the reference lists as without one | (b) Native select per device and point, declared by the protocol; gateway select for the rest | (a) Keep 6.4 literally: Modbus uses the gateway's select only |
| X-2 | 3.3 | Direction and role of a channel | `role: connect \| listen` reserved, phase 2 `connect` only; client or master only | (b) Listening channels now; (c) a server role |
| X-3 | 3.3 | Main and backup connections | (a) A channel owns an endpoint list (room) | (b) A device under two channels; (c) none |
| X-4 | 3.3 | Device addresses on a channel | Point unit id override, shared unit ids without overlapping points | (b) Slices as one device; (d) sub-devices |
| X-5 | 8.10 | Refresh of setpoints and "no audit, no action" | (a) Refreshes belong to the request's record; stop on any failed condition | (b) A record per refresh; (c) no refresh service |
| X-6 | 5.9 | Replayed events | (a) Written in order with STALE, source time may go back, interrogation last; requirements on phases 6 and 7 | (b) Marked, historian only (needs a new path or a layout version) |
| X-7 | 8.4 | Device windows shorter than the operator's select | Per output: `at_select` (default; `select_timeout` at most the window) or `at_operate` (device select and operate back to back at OPERATE) | (a) Gateway select never longer than the device's window |
| X-8 | 4.1 | Per-protocol parts of a template | (b) One `protocol:` per template on a protocol-free base, now; (a) later | (a) now; (c) independent templates |
| X-9 | 8.11 | Safe or release value for remote outputs | (b) The framework can write one, after its own event record; without the record it only stops refreshing | (a) Stop refreshing only |
| X-10 | 2.3 | Interface change in phase 2 | Change now, with defaults | Redesign in phase 9 |
| X-11 | 7.4 | STALE for reported points | "No proof of liveness within `max_age`" | Literal: not refreshed |
| X-12 | 6.8 | Who sets device clocks | Framework activity, default off, gated on the unit's sync; none in phase 2 | (a) Never |
| X-13 | 6.4 | Unservable pacing | Warning | Error |
| X-14 | 5.4 | Enumerated feedback | Double points INT64 0 to 3; `scada-cmd` compares enumerations (phase 5) | Two BOOL tags |
| X-15 | 4.1 | Vendor point lists | Generated files accepted by the schema; importers outside the contract | An importer in phase 2 |
| X-16 | 8.1 | Settings and setting groups | Group switch as an on-only command (`kind: trigger`); settings edits out of scope | Both out of scope |
| X-17 | Annex A | Modbus function codes | 1 to 6, 15, 16 as the minimum set, with a per-device policy | Literal |
| R-1 | 5.8 | Marking of a driver-stamped event time | LOCAL_TIME | TIME_UNCERTAIN; both; none |
| R-2 | 5.6 | Change latches | Latch as a tag | Synthesised on-then-off events |
| R-3 | 2.4 | Reading of the real-time rules (`CLAUDE.md`) for an I/O scan | The bounded device transaction sits unannotated between `SCADA_RT` parts; the system calls of a real-time worker are the table of 2.4 | A separate non-real-time I/O thread feeding the real-time worker (one more hop and its latency); or a change to `CLAUDE.md` |
| R-4 | 8.6 | Reason and raw code packed in `WriteResult.protocol_error` | Packing, read as within "driver-specific" | Raw code only; reason in a diagnostic tag |

### 16.2 Differences from the reference document

1. The `ProtocolDriver` interface has more calls than reference 6.1 (X-10),
   with defaults; `on_unsolicited` is `on_event`.
2. Native select may exist on a protocol the reference lists as without
   one (6.4; X-1).
3. A device may span several unit ids, and several devices may share one
   (12.5; X-4).
4. STALE of a reported point is "no proof of liveness" (6.2; X-11).
5. A driver may write a safe or release value to a remote output (6.5 and
   14.4 give safe states to `scada-io`; X-9).
6. A driver has a command thread besides its channel workers (6.1 names
   only workers), and a channel watchdog that stops the heartbeat.
7. A driver may post event records of its own, for safe writes (phase 5).
8. Request queues are per channel and requester thread,
   `cmd.<C>.<source>`, not `cmd.<driver>.<source>` (5.1): a naming
   difference only, so that each queue keeps one producing thread when
   `scada-control` runs several tasks.

## 17. Change log

| Version | Date | Change |
| --- | --- | --- |
| 0.1 | 2026-11-22 | First draft (issue #99, P2-02), from the outline in `docs/contracts/README.md`, reference sections 6, 8.1 and 14.4, tag database contract 1.0, issue #84, and the device survey of 2026-11-22 (not in the repository). Decisions D-81 to D-92 |

## Annex A. Modbus binding (phase 2)

The protocol-specific rules of the Modbus driver (#105, #106), as far as
the interface depends on them. The full binding schema is #98's.

**Addresses (survey C-18, M-2).** Explicit `table` (`coil`, `discrete`,
`input`, `holding`) and `register` or `bit`, with `address_base: 0 | 1` per
template and no hidden "4xxxx" arithmetic; the result is 0 to 65,535. Unit
ids 0 to 255 on TCP (0 is a normal unit), 1 to 247 on RTU; broadcast only as
an explicit write mode, off by default. Transaction ids are checked, and a
late answer to an earlier transaction is discarded and counted.

**Functions (X-17, M-1).** Reference 6.4's "function codes 1 to 6, 15, 16"
is the set the driver supports, not a set every device has. A device
declares `supported_functions`; the driver substitutes where configured
(16 for 6, repeated 5 for 15, 3 for the input table when a device maps them
alike) and never uses a function the device excludes (`DRV-CFG-07`).
Function 23 only when configured. Not in phase 2 (M-14): ASCII framing,
UDP, vendor functions.

**Types (M-3, M-4).** As C-22 of #98: 16-, 32- and 64-bit integers, 32- and
64-bit floats, 8-bit halves, strings up to 125 registers, byte and word
order separate, values at odd addresses and halves from separate
registers, bit and field points from one register, a value with a validity
bit.

**Block planning (M-6, M-7).** At most 125 registers or 2,000 bits per
read, configurable lower; writes at most 123 registers (some devices
fewer); no merging across tables, even where a device mirrors one table in
another; no bridging of undeclared gaps.

**Exceptions to outcomes (M-8).**

| Answer | On a read | On a write | Demotion |
| --- | --- | --- | --- |
| 01 illegal function | `ABSENT` for the request's points; configuration diagnostic | REJECTED, `NOT_SUPPORTED` | No |
| 02 illegal address | On a merged read, split once (6.3); points refused again `ABSENT` | REJECTED, `INVALID_VALUE` | No |
| 03 illegal value | `ABSENT` with a diagnostic (a read that splits an aligned value) | REJECTED, `INVALID_VALUE` | No |
| 04 server failure | `DEVICE_INVALID`, BAD | REJECTED, `REJECTED_MODE` (a permanent state refusal; not retried) | No |
| 06 busy | Retried at the next turn; no sample | REJECTED, `BUSY` | No |
| 0A, 0B gateway | `COMM_FAIL` for that device only | REJECTED, `COMM_FAIL` | Yes |
| Other codes | `DEVICE_INVALID`, BAD; counted | DEVICE_ERROR, `DEVICE_FAULT` | No |
| Timeout, CRC error, broken frame | `COMM_FAIL` | TIMEOUT, `NO_RESPONSE` (after send) | Yes |

**Transport (M-9, M-10).** TCP: port per channel, keep-alive, reconnect
after an idle close; a refused connection (a connection limit, a device's
client allow-list) is a refusal (`REFUSING`), not a timeout. RTU: the
serial settings of #98, the inter-frame gap from the baud rate, turnaround
time, start-up grace. RTU framing over TCP is a channel `type`
(`rtu_over_tcp`).

**Writes (M-11, M-12).** Function per point; fixed quantity and values;
whole-word masks without read-modify-write; clock blocks in one function 16
request; timeout scaled by the count of values. A device-side select over
coils (one relay in the survey) is a list of writes with roles; its exact
sequence is not known from the documents supplied, so it is room (X-1).

**Record readers (M-13).** Not in phase 2. Registers that move a pointer or
clear on read are marked `side_effect` or `no_read` from the first release,
so that a poll of them cannot corrupt a device's event pointer. Without
record readers the Modbus driver gives no device time stamps.
