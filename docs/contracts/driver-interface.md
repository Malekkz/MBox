# Contract: driver interface

| | |
| --- | --- |
| Contract version | 0.2 (draft) |
| Status | Draft for review by a person (issue #99, P2-02). No code for P2-07 to P2-10 (#104 to #107) starts before that review |
| Implements | Reference architecture 1.1, sections 6, 8.1 and 14.4; tag database contract 1.0, sections 6.3, 9 and 9.5 |
| Owners of this contract | `libscada-driver`; every `scada-drv-<protocol>` and `scada-io` |
| Meets | Configuration schemas contract (`config-schemas.md`, #98), at the points of section 17 |

This contract fixes the boundary between `libscada-driver` (the framework)
and a protocol implementation, and the behaviour of every driver process,
exactly enough that a protocol can be written and tested against the
framework, and the framework against a test protocol, each on its own.

Version 0.x may change freely within phase 2 (contracts `README.md`). The
places marked **open: needs the product owner's ruling** carry a conflict
with the reference document or a choice the reference leaves to a person;
the draft is written with the option proposed in each, and section 18 lists
them all with the committee's recommendation. The places marked **review by
a person** touch command handling, select before operate, fail-safe or
restart behaviour, or secrets (`CLAUDE.md`, "Safety and security rules").

Input from the field: the device survey of 2026-11-22 (not in the
repository). It reviewed manuals of protection relays, inverters, plant
loggers and controllers, meters and serial sensors over several protocols.
Its findings are described here generically; every example is invented.

## 1. Conventions

- **must**, **should** and **may** as in the reference document (section 1).
- **Phase 2** marks what `libscada-driver` (#104), the Modbus driver (#105,
  #106) and `scada-io` (#107) implement. **Room** marks a part whose shape
  this contract fixes now and which a later phase implements; a phase 2
  protocol inherits a default for it. Section 16 lists every part with its
  phase.
- **CS** is the configuration schemas contract (`config-schemas.md`). CS owns
  every configuration key: its name, type, default, range and validation
  code (CF-, CW-), in its table of shared keys (CS 4.10) and its section 10.
  This contract cites keys by name and defines behaviour only. Where this contract names a default, it is CS's default restated,
  and CS wins if the two differ.
- **TD** is the tag database contract (`tag-database.md`), version 1.0. Its
  layout version 1 is frozen (TD 14.1): nothing here changes a shared
  structure, a stored enumeration, a quality flag or a limit that readers
  validate. Where the survey asked for one, this contract gives the
  workaround.
- C++ below shows the interface of `libscada-driver`. Names follow
  `CLAUDE.md`. Every function of the interface is `noexcept`, and every
  `Status` and `Result<T>` is `[[nodiscard]]` (coding standard 7): no
  exception crosses the interface in either direction. A protocol that wraps
  a throwing library catches at its own call site.
- Times are as in TD: `*_time_ns` is `CLOCK_REALTIME` (UTC), `*_mono_ns` is
  `CLOCK_MONOTONIC`. Every timeout, age, deadline and period uses monotonic
  time.
- A **request** is one exchange with a device: one frame sent, its answer
  received or its timeout passed. A **transaction boundary** is the moment
  between two requests of a channel. It is the only point at which a worker
  serves orders, applies pacing, marks STALE, takes a reload, pauses for
  detach or stop, and moves its progress counter. Wherever this contract
  says "at the next boundary", that is at most the end of the current
  request.

## 2. Scope, threads and the protocol interface

### 2.1 What the framework does and what the protocol does

| Concern | Framework (`libscada-driver`) | Protocol |
| --- | --- | --- |
| Configuration | Loads the expanded configuration of its generation (17, B-12), owns the common keys of each point | Validates and compiles its binding (`configure`) |
| Tag database | Attach, heartbeat, notify, every tag write, the restart rewrite (9.1), the generation change | Never calls `libscada-tag`; never sees a handle |
| Transport | Opens, watches and closes the connection; sends; receives; deadlines; frame time; abortive close (7) | Nothing in phase 2. Room: a session protocol owns its transport (2.7) |
| Requests | Schedules every request: orders first, poll groups, pacing, keep-alive, probes, retries, split (6) | Plans the requests of a device; encodes a request; finds the end of an answer; decodes it |
| Samples | Invalid markers, scaling, bits, enumerations, counters, quality rules, STALE, tag writes (5) | Decodes an answer into raw samples with one outcome each |
| Failure | Counts failures, demotes and recovers devices, startup grace (7) | Classifies each answer (value, exception, refusal) |
| Commands | Every check of 8.2, selections, expiry, rate limits, value conversion, results | Encodes the writes of an order; classifies the answer |
| Diagnostics | Publishes every diagnostic tag (11) | Reports counts and codes |
| Reload | Builds the new compiled configuration aside, swaps it at a boundary (12) | Compiles a binding |

Protocol specifics do not pass the framework (reference 3.3): above the
driver there are only tags and write requests. No type of a protocol library
appears in this interface (reference 16.3).

### 2.2 The unit of the interface is one request

Decision D-93. The framework owns the device exchange; the protocol
supplies its planning, encoding and decoding as pure functions. Pacing,
writes first, retries, the split of a refused block, keep-alive, the
exclusive sequence of a select and operate, and the counts per request are
then written once, in the framework, and every protocol gets them.

```cpp
namespace scada::driver {

// A frame protocol: request and answer over a transport the framework owns
// (TCP, serial, RTU framing over TCP). Phase 2: Modbus TCP and RTU.
// The object holds no mutable state. It is called from the main thread and
// from every channel worker at once; all mutable state lives in the
// compiled objects it builds (DeviceState) and in the framework's objects.
class FrameProtocol {
public:
  virtual ~FrameProtocol() = default;

  // Main thread, or scada-conf at validation (17, B-12). Allocates; no I/O.
  // Validates every binding and compiles it into a new CompiledConfig,
  // built aside: the running configuration is not touched. Errors go to
  // `errors`, each with file, line and a CS code.
  [[nodiscard]] virtual Status configure(const DriverConfig& config,
                                         CompiledConfig& out,
                                         ConfigErrors& errors) const noexcept = 0;

  // Main thread. Static facts of a device and of each of its points (2.8).
  [[nodiscard]] virtual DeviceCaps device_caps(const CompiledDevice& d) const noexcept = 0;
  [[nodiscard]] virtual PointCaps  point_caps(const CompiledPoint& p) const noexcept = 0;

  // Main thread. Groups the points of each poll group into requests within
  // the read rules (6.3). Each request carries its split form: the requests
  // it becomes if the device refuses it as a whole. Also names the device's
  // keep-alive and probe requests (6.5, 7.4). Allocates.
  [[nodiscard]] virtual Status plan(const CompiledDevice& d, const ReadRules& rules,
                                    ReadPlan& out) const noexcept = 0;

  // Channel worker. Encodes one request (a planned read, or one write of an
  // order) into `out`, a buffer of the framework sized at configure.
  SCADA_RT [[nodiscard]] virtual EncodeResult encode(const CompiledDevice& d,
                                                     const Request& r,
                                                     TxBuffer& out) const noexcept = 0;

  // Channel worker. Given the bytes received so far for `r`, says whether
  // the answer is complete (and its length), needs more bytes, or is not
  // valid. Never reads beyond `received`.
  SCADA_RT [[nodiscard]] virtual FrameState frame_state(const Request& r,
                                                        std::span<const std::byte> received) const noexcept = 0;

  // Channel worker. Decodes a complete answer into raw samples (5.1), or
  // into the outcome of a write. `state` is the device's mutable protocol
  // state (a transaction id), owned by the framework's Device.
  SCADA_RT [[nodiscard]] virtual DecodeResult decode(const CompiledDevice& d,
                                                     DeviceState& state,
                                                     const Request& r,
                                                     std::span<const std::byte> frame,
                                                     SampleSink& out) const noexcept = 0;

  // Channel worker. Bytes that arrive with no request outstanding (an
  // unsolicited frame of a later protocol). Default: discarded and counted.
  SCADA_RT virtual void unsolicited(const CompiledDevice& d, DeviceState& state,
                                    std::span<const std::byte> frame,
                                    SampleSink& out) const noexcept;
};

}  // namespace scada::driver
```

**open: needs the product owner's ruling (X-10).** Reference 6.1 shows a
`ProtocolDriver` with `connect`, `disconnect`, `poll(Device&, PollGroup&,
Samples&)`, `on_unsolicited`, `write` and `read_now`, in which the protocol
does its own I/O and a poll group is one call. That shape lets the
framework act only between poll groups, so every protocol would implement
pacing, retries and writes-first itself (committee review). This draft
replaces it in phase 2 with the frame protocol above, in which the
reference's calls become framework functions (`connect` and `disconnect`
of the transport, the scheduled poll, `read_now`, the writes of an order)
built on `encode`, `frame_state` and `decode`. Committee recommendation:
accept.

The interface of `scada-io` is the HAL of reference 6.5 (section 13), not
a frame protocol.

### 2.3 Calls, threads and bounds

| Call | Thread | May block on | Bound | `SCADA_RT` | Phase 2 Modbus |
| --- | --- | --- | --- | --- | --- |
| `configure` | Main, or `scada-conf` | Nothing; allocates | None (outside every cycle) | No | Must |
| `device_caps`, `point_caps` | Main | Nothing | None | No | Must |
| `plan` | Main | Nothing; allocates | None | No | Must |
| `encode` | Worker | Nothing | Pure computation over one request | Yes | Must |
| `frame_state` | Worker | Nothing | Linear in the bytes received, at most the largest legal frame | Yes | Must |
| `decode` | Worker | Nothing | Linear in the frame | Yes | Must |
| `unsolicited` | Worker | Nothing | Linear in the frame | Yes | Default |

No call of the protocol makes a system call, takes a lock, allocates or
logs. A protocol reports through return values and the sink, and the
framework logs (11.3).

### 2.4 Threads

| Thread | Count | Runs | Tag owner (CS 12.3) | Scheduler |
| --- | --- | --- | --- | --- |
| Main | 1 | Attach, heartbeat and the service manager's watchdog, the generation change, reload, the channel watchdog (2.6), process-level diagnostics, logging (11.3), signals, and from phase 5 the one producer of the process's events queue (8.13) | `drv-<protocol>` | Normal |
| Command | 1 | Takes every request from the `cmd` queues of the process (`scada_queue_next`), the checks of 8.2 that need no device, dispatch to the workers, every answer, the fail-safe triggers of 8.13 | None | Normal |
| Channel worker | 1 per channel | Every request of its channel: connect, sends, receives, writes; every tag write of the channel's devices; `scada_tag_notify` after each delivery | `drv-<protocol>.<C>` | Normal in phase 2 (2.5) |

- One owner per channel worker: an owner is one writing thread (TD 2), one
  channel one owner (reference 5.1). A device's point tags and its
  `sys.dev.<device>.*` tags, and the channel's `sys.drv.<channel>.*` tags,
  are written by the channel's worker; `sys.proc.<process>.*` by the main
  thread.
- **Request queues** are per driver process and source, as reference 5.1
  names them: `cmd.<protocol>.<S>` with its answer queue
  `result.<protocol>.<S>`, where `<S>` is `cmd` for `scada-cmd` or the
  short name `<T>` of a control task (CS 12.3). One queue per source keeps
  one producing thread per queue (TD 9). The command thread consumes them
  all with one wait set; a wait set holds at most 127 sources (TD 8.4), so
  a driver may have at most 126 sources, and CS refuses more (CF-1009). The command
  thread routes each request to its channel by the target handle.
- **In-process queues.** Between the command thread and each worker there
  are two pre-allocated single-producer, single-consumer lock-free queues:
  orders to the worker, outcomes back. Both have the capacity
  `max_pending_writes` of the channel (CS). A full order queue is answered
  at once REJECTED with reason `BUSY` (8.6): the command thread never waits
  on it. The worker posts exactly one outcome per order, so the outcome
  queue can never be full when the worker posts (proof: an order occupies
  an order slot until its outcome is posted, and both queues have the same
  capacity). Everything else the command thread needs from a worker is
  published in atomics, not queued: the device state of 3.4, and a
  connection epoch per channel, incremented at every connect, which a
  native selection records (8.4, rule 3).
- **Command thread wake.** The command thread waits on its wait set and on
  an eventfd that every worker signals after posting an outcome; it uses
  the wait set's descriptor (`scada_waitset_fd`, TD 13) and the eventfd in
  one `poll`.
- **Signals.** Every thread but the main thread blocks all signals; the main
  thread takes them synchronously (`signalfd`). A stop or reload signal
  never interrupts a worker's wait.
- **Stacks and memory.** A worker's stack is 256 KiB, set at thread
  creation. A driver process locks its memory (`mlockall`) only when it has
  a real-time thread; in phase 2 only `scada-io` does (2.5).
- **Limits a driver unit needs**, as a function of its channel count N and
  its source count S: descriptors about 2N + 4S + 32 (a socket or port and
  an eventfd per worker, the queue mappings of TD 9.2), threads N + 2. CS
  computes them and the install tree sets them in the unit (#105).

Decision D-82.

### 2.5 Real-time tier in phase 2

**open: needs the product owner's ruling (R-3).** Committee recommendation:
accept.

`CLAUDE.md` says that code running in "the I/O scan" must not "make system
calls ... or do file or network I/O". An I/O scan cannot exchange anything
with hardware without the system calls of that exchange. This draft
proposes, decision D-83:

- **Phase 2: the real-time tier is `scada-io` only** (reference 4.3 names it).
  Modbus workers run under the normal scheduler. Reference 7.5 already gives
  Modbus a realistic control period of 50 ms to several seconds, and a
  10 ms scan over RTU is not reachable at common rates (15 bytes take
  8.6 ms at 19,200 bit/s). Real-time protocol drivers and synchronous scan
  groups are room, added by a later minor version of this contract and of
  CS together (6.7).
- In a real-time thread, the functions that are not exchanges with
  hardware are `SCADA_RT`: in `scada-io`, the HAL backend's decode, scaling
  and filtering, the sample delivery and the tag writes, the take of
  orders from the in-process queue, the outcome hand-over. The exchange
  itself (the HAL's `read_inputs` and `write_outputs` system calls of 13.3)
  sits between them, unannotated, with its bound stated per backend and
  channel.
- `CLAUDE.md` and the coding standard are not changed by this contract.

What enforces the rules, honestly stated:

- **The `SCADA_RT` check** on `main` finds direct calls, inside a function
  marked `SCADA_RT`, to a deny-list: allocation functions, C stream
  output, standard mutexes. It does not follow calls into other functions,
  and does not know exceptions, `std::function`, `std::string`,
  `Logger::log`, blocking calls or system calls. A marked function that
  calls an unmarked helper which allocates passes the check. The annotation
  is therefore a marker for review and for the two tests below, not a
  proof.
- **Allocation test** (conformance test 3): an allocation counter per
  thread, at the level of `malloc` and of `operator new`, over the whole
  real-time thread for 10,000 cycles, including its error paths (a HAL
  read error, a full outcome queue, a stop). Expected: 0.
- **System-call trace** (conformance test 29): a trace of the real-time
  thread's system calls over the same cycles and error paths; the allowed
  set is the table of 13.3 for that backend. The test proves which calls
  were made on the paths it drove, and records the longest time spent in
  each under load; it proves nothing about paths it did not drive.
- Both tests are exempt from the sanitizer builds (the sanitizers allocate
  and make system calls on every thread) and run on the native runners of
  both architectures, never under user-mode emulation, where `ptrace` and
  `seccomp` do not work.
- `scada_tag_notify` makes system calls once a second and once a minute on
  the calling thread (#116, point 3). On a real-time thread it is **open:
  depends on #116 point 3**: until a wake-only form exists (an interface
  addition, TD 1.1), the real-time thread of `scada-io` calls
  `scada_tag_notify` once per cycle after its writes, and the trace lists
  its calls as allowed, so the test cannot tell them from a stray call.

In a Modbus worker (normal scheduler) `encode`, `frame_state` and `decode`
are still `SCADA_RT`: they run in any later real-time worker unchanged, and
the allocation test runs on the test protocol's worker too.

### 2.6 Channel watchdog

The heartbeat is per process (TD 10.1). A worker that stalls while the main
thread beats would leave its tags with their last values and real age,
never OWNER_DOWN (#116, comment point 1). Decision D-84:

- Each worker increments a progress counter (an atomic in process memory)
  at every transaction boundary, and at every wake of its wait with
  nothing due, so that an idle channel is not taken for a stalled one.
- The main thread checks every counter at each heartbeat. A counter that
  has not moved for three times the longest single request deadline of the
  channel (the largest device `timeout` plus frame time, at least 1 s) is a
  stall.
- On a stall the main thread logs it (11.3, with the channel name), stops
  the command thread and the other workers at their next boundary (bounded
  by their request deadlines, at most 1 s beyond), stops both
  `scada_tag_heartbeat` and the service manager's watchdog notification,
  and ends the process with `std::_Exit` and a distinct exit status (coding
  standard 7, D-59). It never kills or cancels a thread. The service
  manager restarts the process.

**open: needs the product owner's ruling (O-3).** One stalled channel
restarts every channel of its protocol: only the stalled thread may write
its tags, so the process must end for them to read OWNER_DOWN. With 300
Modbus devices in one process, one hung serial adapter sets every Modbus
tag OWNER_DOWN and ends every selection and refresh; a stall that repeats
runs the unit into the restart limit of reference 14.4, after which no
Modbus device is polled. The real fix is per-owner liveness (#116). Room:
a channel key that puts channels in separate processes of one protocol
(`drv-<protocol>-<group>`), CS reserved. Committee recommendation: accept
for phase 2, with the room.

### 2.7 Session protocols and report-driven protocols (room)

Phase 9 protocols built on a third-party stack own their transport and may
own threads (OPC UA, IEC 61850 MMS). They implement a second interface,
`SessionProtocol`, with the shape below; no phase 2 code implements it.
Decision D-82.

- The worker is an event loop over the session's descriptors and timers,
  the order queue's eventfd and the next scheduled activity. It calls the
  protocol's `on_ready`, `run_activity` and `start_write`, none of which
  blocks.
- A stack with its own threads runs them as protocol helper threads. A
  helper never writes tags. It hands samples to the worker through one
  bounded single-producer queue per helper thread
  (`Channel::remote_sink(helper_index)`), and signals the worker's
  eventfd. A full sample queue drops the samples and counts them
  (`samples_dropped`), and the framework treats the drop as a buffer
  overflow (5.7). Write completions go through reserved slots, one per
  pending order, which can never be dropped.
- A write completes through a `CompletionToken` passed by value (the order
  slot and its sequence number). The framework honours exactly one
  completion per token; a completion for a slot that has moved on (the
  order timed out, the generation retired) is discarded and counted. No
  reference to a framework object outlives the call that passed it.
- Frame protocols that receive unsolicited frames (IEC 104, DNP3) stay
  frame protocols and use `unsolicited` and activities (6.1).

### 2.8 Capabilities

Static facts the framework schedules and checks by. Each has a default, so
a phase 2 protocol sets only what applies.

| Fact | Per | Values | Default | Used in |
| --- | --- | --- | --- | --- |
| `max_read_units`, `max_write_units` | Device | Count of protocol units in one request | The protocol's limit | 6.3 |
| `max_frame_bytes` | Device | Largest legal answer | Protocol limit (Modbus TCP 260, RTU 256) | 7.1 |
| `select` | Point | `gateway`, `native` | `gateway` | 8.4 |
| `native_select_window` | Point with native select | Duration | None | 8.4 |
| `write_completion` | Point | `immediate`, `two_phase` | `immediate` | 8.6 |
| `writable_shape` | Point | Whether the point's raw shape can be written (8.3) | From the binding | 8.3 |
| `acquisition` | Point | `poll`, `report` | `poll` | 6.1 |
| `activities` | Device | Kinds with default periods | None | 6.1 |
| `time_sync` | Device | None, or the clock forms the protocol can set | None | 6.8 |

## 3. Object model

### 3.1 Objects

| Object | What it is | Built | Replaced or destroyed | Owned by |
| --- | --- | --- | --- | --- |
| `Driver` | The process's framework instance and its protocol | Process start | Process exit | `main` |
| `CompiledConfig` | Every compiled channel, device, point, poll group and output of one configuration version | `configure`, on the main thread | By the next one at reload (12); the old one destroyed on the main thread | `Driver` |
| `Channel` | One connection or port and its worker | With its `CompiledConfig` | Reload, exit | `Driver` |
| `Device` | One addressable device on a channel: timeout, retries, demotion state, identity, its `DeviceState` | With its channel | Reload, exit | Its `Channel` |
| `Point` | One configured signal of a device, bound to one tag (or none, for a point that only feeds others) | With its device | Reload, exit | Its `Device` |
| `PollGroup`, `Request` | A period and priority with its points; the planned requests | `plan` | Reload | Its `Channel`, its `Device` |
| `Output` | The write side of a point (section 8) | With its point | Reload | Its `Point`; read by the command thread (3.2) |

- A protocol's compiled form of a device or point derives from a framework
  base class with a virtual destructor (`ProtocolData`), held by the
  framework object in a `std::unique_ptr`. Its mutable part
  (`DeviceState`: a transaction counter, a session flag) is a separate
  object of the device, written only by the channel's worker.
- A protocol keeps no pointer or reference to a framework object beyond the
  call that passed it.

### 3.2 Configuration in use, and its replacement

- The worker and the command thread each read the configuration through an
  atomic pointer to the current `CompiledConfig`, loaded with acquire at
  each boundary (worker) and at each request (command thread).
- A reload builds a new `CompiledConfig` aside on the main thread (12). The
  main thread publishes it with a release store; each worker and the
  command thread take it at their next boundary and acknowledge with an
  epoch counter. When all have acknowledged, the main thread destroys the
  old one. No real-time or worker thread ever frees configuration memory.

### 3.3 One point is not one address

The model allows (survey D-1): one read delivering many points; one point
from several protocol units in a stated order, possibly not adjacent; a
point with companions (an exponent, a status, a time source, 5.5); several
bit or field points from one word; one command writing an ordered list of
protocol writes (8.3).

### 3.4 Devices on a channel

- Several devices share one channel, each with its own timeout, retries,
  demotion state and diagnostics. The worker serialises their requests: a
  timeout of one device delays the others by one request at most (survey
  D-3).
- A lost connection fails every device of the channel at once (7.3).
- **open: needs the product owner's ruling (X-3).** Main and backup
  connections: a channel owns a list of endpoints, one active (room; phase 2
  one endpoint). A device stays under one channel.
- **open: needs the product owner's ruling (X-4).** A device has a default
  unit id; a point's address may name another (one device answering on
  several unit ids); several devices may share a unit id when their points
  do not overlap (slices of one gateway unit). The overlap rule of reference
  12.5 then applies to (unit id, table, register) (CS). Devices that share a
  unit id also share its failure count (7.4).
- **open: needs the product owner's ruling (X-2).** Channel `role: connect |
  listen` (CS); phase 2 `connect` only. Every driver stays client or master
  (reference 6.4).
- Other masters may share a device. The framework takes the feedback tag as
  the truth and never assumes it is the only writer (8.9).
- One owner per channel caps a unit at 1,023 channel owners (TD 14). Room:
  several connections per worker, for sites of many single-connection TCP
  devices.

### 3.5 Device state

Published as `sys.dev.<device>.state` (INT64):

| Value | State | Meaning | Tags of the device |
| --- | --- | --- | --- |
| 0 | `DISABLED` | Disabled in configuration (CS) | Exist; points BAD with NOT_INIT; nothing is sent |
| 1 | `CONNECTING` | Channel not connected, or the connection was refused | BAD, COMM_FAIL after the first failed attempt, NOT_INIT before |
| 2 | `INITIALISING` | Connected; identity check, session steps (room) or `startup_grace` running | Kept; BAD with NOT_INIT if never read |
| 3 | `RUNNING` | Polled | From samples (5) |
| 4 | `DEMOTED` | Failures reached `demote_after` (7.4) | BAD, COMM_FAIL |
| 5 | `MISMATCH` | Identity check failed with `on_mismatch: refuse` (3.6) | BAD, no COMM_FAIL; writes refused |
| 6 | `REFUSING` | Answers on an open connection that refuse service: a gateway that has no route, a server function off, a device not started (annex A) | BAD, no COMM_FAIL |

Only the framework changes the state; the worker publishes it in the tag
and in an atomic the command thread reads (2.4). A refused TCP connection
is `CONNECTING` with COMM_FAIL and is counted apart
(`sys.drv.<channel>.refused_connects`): a rebooting device refuses
connections too, and its tags must keep COMM_FAIL.

### 3.6 Identity

- Configured identity (CS 4.8): points or protocol identity objects with an
  expected value, list, range or pattern, and `on_mismatch` (CS default).
- The framework reads the identity points after every connect and after a
  detected device restart (7.6), compares, publishes
  `sys.dev.<device>.identity` (STRING) and `sys.dev.<device>.identity_ok`
  (BOOL), and on a mismatch with `refuse` sets `MISMATCH`.
- A `pattern` is the pattern form of CS (linear matching time, no general
  regular expression): the text compared comes from the device.
- `MISMATCH` is left by a reload of the device or a later check that
  matches. A device in `MISMATCH` answers every write REJECTED with
  `CONFIG_MISMATCH` and is read only for its identity points, at
  `backoff`.

Phase 2.

## 4. Point addressing

- A point has common keys (CS 4.7), processed by the framework, and one
  protocol key, `address`, opaque to the framework. Channels and devices
  have framework keys and protocol keys side by side (CS 4.6); in this
  contract the protocol part is the **binding**.
- **open: needs the product owner's ruling (X-8).** Both drafts propose
  option (b) for phase 2: a template carries one `protocol:` and may
  `extends:` a protocol-free base (CS 4.1). Option (a), one template with a
  binding per protocol, is room. The driver side is the same under both.
- The protocol receives each binding as a typed configuration tree that
  carries file and line for every value.
- `configure` checks every field against the protocol's limits and refuses
  with a located CS code (CF-650 to CF-699, listed per protocol in CS); it never falls back to a default the
  configuration did not give. It reports each point's companions and the
  raw units it reads.
- The same `configure` runs in `scada-conf` at validation and in the driver
  when it loads its configuration (17, B-12), so the two cannot disagree.
- A symbolic address that needs the device compiles to "unresolved"; a
  resolution step after connect is room (survey D-6). An unresolved point
  is `ABSENT` (5.2).

## 5. Samples and quality

### 5.1 Raw samples and the processing after decode

`decode` delivers **raw samples** to the `SampleSink`. A raw sample names a
point and carries:

- one outcome (5.2);
- the raw pattern: the value's bits after the binding's byte order, word
  order and parts are applied, before any numeric interpretation, up to 64
  bits (CS 4.7, annex A); and the raw number the protocol read from it with
  the binding's raw type, or a byte string for a string point;
- an optional device time (5.8);
- an optional protocol status code (`int32`);
- room: a cause and a replay mark (5.9).

Bit and field extraction named in the `address` (`bit`, `field`) is part of
the protocol's decode. Everything after decode is the framework's, in this
order (CS owns the keys):

1. **Invalid markers** (`invalid_values`, CS 4.7) on the raw pattern. A
   marker of an integer raw type is the pattern of the raw width; a
   negative decimal marker is its two's complement in that width; a float
   marker is an exact pattern or "any NaN". The match applies its
   `on_invalid` action.
2. **Non-finite values.** A float raw value that is NaN or infinite and
   matched no marker is written BAD with no flag and counted
   (`sys.dev.<device>.invalid`). A non-finite value is never written GOOD.
3. **Exponent** (`exponent_from`): the companion's integer e, checked
   against its range (CS); a value outside it gives BAD. For e ≥ 0 the
   value is multiplied by 10^e; for e < 0 it is divided by the exact
   binary64 value of 10^|e| (exact up to 10^22; CS bounds e). Exponent
   before scale.
4. **Scale** (`scale`): `raw x gain + offset`, or `raw / divide_by + offset`.
   - With an integer gain and offset the tag is INT64 or UINT64 (CS 4.7),
     and the arithmetic is checked: CS refuses a configuration whose raw
     range times the gain cannot fit; a result that still does not fit
     (an offset at the edge) gives BAD with OVERFLOW.
   - Otherwise the tag is FLOAT64, and the computation is in binary64 with
     floating-point contraction off (decision D-98), so that both
     architectures give the same bits.
5. **Bits, enumerations, double points, counters** (5.4, 5.6).
6. **Quality** from the outcome, `quality_from`, health rules and
   `raw_range` (5.2, 5.5).
7. **Time** (5.8).
8. **The write**, one `scada_tag_write` per sample. Several samples of one
   point in one delivery are written one by one, in the order given, so
   the change ring, alarms and the historian see each (survey D-11).

The bounds of the sink are set at configure from the plan: a delivery never
grows a container.

Golden vectors (conformance test 5) fix this arithmetic: raw bytes plus a
binding give value bits, quality and flags, bit-identical on both
architectures. The vectors are shared by #104, #105 and the simulators.

### 5.2 Outcomes

| # | Outcome | Meaning | Quality | Flags | Demotion |
| --- | --- | --- | --- | --- | --- |
| 1 | `VALUE` | The device answered with a value | GOOD, unless steps 1, 2 or 6 of 5.1 lower it | As those steps give | Clears the failure count |
| 2 | `NO_VALUE` | The device answered, but with a "no value" marker | BAD | The marker's flags | No |
| 3 | `ABSENT` | The point is not on this device: the device refused the address, or it is unresolved | BAD | NOT_INIT if never read | No |
| 4 | `DEVICE_INVALID` | The device reported the value invalid: a status, a validity bit, a health rule | BAD or UNCERTAIN by the mapping | The mapping's flags: BLOCKED (out of service), TEST (test or simulation), OUT_OF_RANGE, OVERFLOW, SUBSTITUTED (forced at the device) | No |
| 5 | `COMM_FAIL` | No usable answer: timeout, broken frame, lost connection, a gateway that cannot reach its device | BAD | COMM_FAIL | Counts (7.4) |

- A tag whose sample is not `VALUE` keeps its last value, or 0 if it never
  had one. Readers must not use the value of a BAD tag (TD 6.2).
- An `ABSENT` point is counted in `sys.dev.<device>.points_absent`, logged
  once per connection in a summary (11.3), and requested again only at
  `backoff`.
- SUBSTITUTED: TD 6 defines it as "value entered by a person in place of
  the source". This contract also sets it for a value forced at the device
  and for a substitute from configuration (`on_invalid: substitute`). That
  widens a frozen flag's meaning: **open: needs the product owner's ruling
  (R-6)**, with R-4. Committee recommendation: accept, with HMI text that
  says "substituted", never "entered by an operator".

What does not fit the frozen tag database, and how it is carried:

| Detail | Carried as |
| --- | --- |
| Protocol quality detail without a flag | Folded into GOOD, UNCERTAIN or BAD; a raw-quality companion tag (UINT64) is room (#119) |
| "Not on this variant" | No tag where configuration knows it (`when`, `disable`, CS); otherwise `ABSENT` |
| Device disabled, failed, in local mode | `DEVICE_INVALID`, BAD, no COMM_FAIL, plus the device's health points |
| Configuration or permission fault | `REFUSING` (3.5) and `sys.dev.<device>.last_status` |
| Strings over 250 bytes | Truncated to 250 bytes at a UTF-8 boundary, with OVERFLOW |

### 5.3 Invalid markers and raw range

As step 1 of 5.1, with the keys and actions of CS 4.7 (`invalid_values`,
`on_invalid`, `raw_range`). The flags each action sets are CS's.

### 5.4 Tag types

- Raw integers with an integer gain and offset stay INT64 or UINT64;
  anything else is FLOAT64 (5.1, step 4).
- Bits are BOOL; enumerations INT64 with the table of CS.
- **Double points** are INT64 with one coding for every driver: 0
  intermediate, 1 open or off, 2 closed or on, 3 bad state. Value 3 is
  written as 3 with BAD (so the HMI shows "faulty", not "no data"); 0 stays
  GOOD. The address shapes (two bits, three bits, two coils, a 2-bit field,
  an integer) are CS's; the protocol maps them onto this coding.
- Device times are INT64 nanoseconds UTC.
- **Counters** (`kind: counter`, CS 5.3). The tag holds the device's counter
  value, scaled; never a total continued across wraps or restarts (no
  durable state in phase 2). A value at `limit` with `at_limit: saturate`
  sets OVERFLOW. Under `at_limit: wrap` a decrease from at least `limit`/2
  to below `limit`/2 is a wrap: the restarted value is written with no flag.
  Any other decrease applies `on_decrease` (CS): `accept` writes it; `hold`
  keeps the previous value with UNCERTAIN until the counter passes it;
  `reset` writes it and counts a reset.
- Strings are STRING tags, at most 250 bytes (TD 7). `scada_tag_write_string`
  is not RT-safe (TD 13): no STRING point on a real-time thread.

### 5.5 Companions and health

- A point may depend on companions (CS 4.7): an exponent, a status word or
  validity bit, a time source. The planner reads a point and its companions
  in one poll of one poll group, back to back with no other request to that
  device between them (one request when they fit in one). A companion with
  any outcome but `VALUE` makes the dependent sample BAD with that outcome's
  flags.
- `quality_from` maps a companion's bit, field or value to a point's
  quality; a device `health` rule applies to all or a named group and states
  whether it sets COMM_FAIL (CS 4.7). A health point read in a slower poll
  group applies its last value; a health point that is itself BAD makes the
  points it governs UNCERTAIN.

Phase 2.

### 5.6 Bits, fields and change latches

- One raw unit may deliver several bit or field points; only named bits
  create tags. The numbering is CS's `bit_order`, which has no default and is required wherever a bit or field is used.
- A change latch (a bit that records "changed since last read", cleared on
  read) is its own BOOL tag, read like any bit of a `side_effect` range
  (6.6). The framework never synthesises an on-then-off event from a latch
  and a state that has returned: the time of such an event would be
  invented. Decision D-87.

### 5.7 Lost events

A protocol that learns that the device dropped events, or that its own
buffer overflowed, reports it through the sink (`events_lost`). The
framework counts it, logs it and runs the device's integrity read. Room;
phase 2 Modbus has no event buffers.

### 5.8 Time of a sample

| Case | `source_time_ns` | Flags |
| --- | --- | --- |
| No device time (every polled Modbus point without a time companion) | The receive time of the answer | LOCAL_TIME |
| Device time from a companion (CS `source_time_from`, `epoch`, `datetime`), `device_time.trust: synced` | The device time, converted to UTC | None |
| The same with `trust: unknown` (the CS default), or a local time in the repeated hour at the end of daylight saving time, or in the skipped hour | Converted; the repeated hour takes the earlier offset; the skipped hour is moved forward by the gap | TIME_UNCERTAIN |
| An event whose device gave no time, stamped by the driver | The receive time | LOCAL_TIME |
| The unit's own clock is not synchronised (reference 14.4) | As above | TIME_UNCERTAIN added (room: the time service's tag, phase 7) |

- Driver-stamped event time is marked LOCAL_TIME: that is the flag's
  meaning in TD 6 ("`source_time_ns` was set by the owner, not a device").
  TIME_UNCERTAIN keeps one meaning: a clock that may be wrong. Decision
  D-86 (R-1). Committee recommendation: accept.
- Device times from companions are phase 2. Event records and their device
  times are room (annex A).
- A polled value read again unchanged is a fresh sample: it refreshes the
  age and clears STALE.

### 5.9 Replayed events and cause (room)

**open: needs the product owner's ruling (X-6), deferred.** Phase 2 has no
protocol that replays events. The raw sample keeps room for a cause and a
replay mark (5.1). The choice is deferred to the historian contract (phase
3) and the alarm contract (phase 7). Decision D-88. The options and what
they cost:

| Option | What consumers see | Cost |
| --- | --- | --- |
| (a) Write each replayed event to its tag in device-time order, interrogation last; add STALE to replayed samples | A tag's `source_time_ns` may go backwards; a control block sees STALE and applies its bad-or-stale behaviour, so an old trip drives no logic | If the alarm engine raises no alarm from a STALE sample, a trip that happened during the outage raises none either: hidden. If it does, an old trip raises an alarm late, with its source time |
| (a') As (a) without STALE | An old trip is in the live tag for at least one control cycle | A replayed trip can drive logic once |
| (b) Mark replayed samples for the historian only; the live tag only when newer | The live tag never goes back in time | Needs a path to the historian that bypasses the tag, or a quality bit: a new path or a layout version |

Committee recommendation: defer, as drafted.

## 6. Scheduling

### 6.1 Poll groups and activities

- A channel has poll groups with a period and a priority (CS 4.6). A point
  is in one poll group, or in none (`access: wo`, a report-only point).
- Room: activities a protocol registers per device (integrity and class
  polls, record readers, time sync, session steps), scheduled by the
  framework like poll groups.

### 6.2 Order of service

At each transaction boundary the worker chooses the next request:

1. The next step of an order in progress (a list of writes, select then
   operate) when its sequence is exclusive (6.6).
2. A new order (write, select, operate, cancel, `READ_NOW`), in arrival
   order, when the device's write spacing allows (6.4).
3. The next request of a due poll group, by priority, then by due time,
   when the device's request spacing allows.
4. A keep-alive request (6.5).
5. A probe of a demoted device, within the probe budget (7.4).

When nothing is allowed yet the worker waits for the earliest of: the next
spacing end, the next due time, an order (its eventfd), the connection
becoming readable (7.3).

### 6.3 Block planning

- Rules (CS `read:`, annex A.4): the largest request (`max_registers`, at most
  `max_read_units`), the largest gap the plan may read across (`max_gap`),
  and ranges per table and unit id: `no_read` (never touched),
  `atomic` (always in one request), `exclusive` (read alone, by one poll
  group), `side_effect` (6.6).
- The plan never splits a multi-unit value, never separates a point from
  its companions (5.5), never merges across tables, unit ids or address
  spaces. What a bridged gap returns is never decoded.
- Each planned request carries its split form. When the device refuses the
  whole request as an illegal address (annex A), the framework uses the
  split form once: the points refused again are `ABSENT`; the rest keep the
  split plan until reconnect.
- The plan is published (`sys.dev.<device>.requests_per_scan`, the requests
  of one period of every poll group of the device) and logged at configure.

Phase 2.

### 6.4 Pacing

- Per device and channel: `min_request_interval`, `min_write_interval` and
  `max_values_per_request` (CS 4.6). The worker never sends two requests to
  one device, or two writes through one channel, closer than its interval.
  "Writes first" keeps the write interval.
- A device paced at 10 s does not delay its neighbours: the worker serves
  other devices while one waits.
- The framework publishes the achieved period per poll group
  (`sys.drv.<channel>.period.<group>`, ms) and `sys.dev.<device>.pacing_limited`.
- **open: needs the product owner's ruling (X-13).** An unservable poll
  group is a warning (CS code), not an error: the runtime comparison of
  measured and configured periods already alarms (reference 7.5). CS owns
  the severity. Committee: the reviewers differ; the draft keeps warning.

### 6.5 Keep-alive

A device or channel with `keepalive` (CS) that has sent no request for that
period gets one: the keep-alive request of its plan, which `plan` picks
outside every `side_effect`, `exclusive` and `no_read` range. It is a
protocol request; TCP keep-alive probes are not used in its place (a device
that counts protocol requests for its idle timer ignores them). Phase 2.

### 6.6 Side effects and exclusive sequences

- A `side_effect` range (CS `read:`: a read clears flags, moves a pointer or
  restarts a statistic) is read once per poll of its poll group. Every point
  decoded from it, a value, a status word that qualifies other points, a
  latch, is served from that one read, and must be in that poll group (CS).
  Merging it with other points of the same poll group is allowed.
- A request that touches a `side_effect` range is never retried, never sent
  by `READ_NOW`, keep-alive, identity or probe. A timeout on it loses that
  poll's flags; the next poll reads again.
- An `exclusive` range is read alone, by one poll group (record pointers).
- An exclusive sequence (a list of writes, a select and operate) runs on one
  device with no other request to that device between its steps.
- A second master on the same device can still break either. The framework
  cannot detect it.

### 6.7 Synchronous scan groups (room)

Reference 6.2's synchronous scan group (the driver signals the end of a
scan and a control task runs once per scan) needs a real-time driver
(2.5). Room in both contracts (CS reserves `scan_group`). The shape kept
for the later minor version: the worker writes a scan counter tag at the
end of each scan of the group, then notifies; a task waits on that owner's
change ring, not on the process's `cycle_seq`, which counts every notify of
every channel (TD 10; #116, point 4).

### 6.8 Time sync of devices (room)

**open: needs the product owner's ruling (X-12).** A framework activity per
device, default off, never while the unit's own clock is not synchronised.
CS reserves `time_sync`, `none` only in phase 2. Committee recommendation:
accept.

## 7. Transport and failure handling

### 7.1 Timeouts and the frame time

Decision D-96.

- A device's `timeout` (CS 4.11) is the time from the **end of the request on
  the wire** to the **first byte of the answer**.
- The framework adds the frame time: the request's length and the longest
  answer the request allows, at the channel's bit rate and character format
  (start, data, parity and stop bits), plus a margin of 2 characters. On
  TCP the frame time is 0. A request's deadline is its send time plus its
  frame time plus `timeout`. Example: a read of 125 registers at 1,200 bit/s
  in 8E1 is 8 bytes out and 255 back, 0.07 s and 2.34 s: with `timeout:
  1s` the deadline is 3.4 s after the send.
- CS warns when the requests of a poll group cannot fit its period at the
  channel's bit rate.
- A read that times out is retried up to `retries` times, within its poll
  group's turn, unless it touches a `side_effect` range (6.6). A write is
  never retried (8.7).
- The bytes accepted for one answer are bounded by `max_frame_bytes`; more
  bytes close the connection (a TCP channel) or discard input and wait the
  quiet time (a serial channel), and count as `COMM_FAIL`.

### 7.2 Serial lines

- The worker never drains the line and never toggles a modem line inside
  an exchange: user space cannot meet the RS-485 turnaround that way. It
  computes the end of transmission from the frame length and the bit rate.
  Direction control is the adapter's (`rs485.direction: auto`) or the
  kernel's RS-485 mode set once at open (CS reserves the `rs485` keys).
- The end of an answer is found by `frame_state` from the function code,
  the length field and the CRC, never from a gap: user space does not see
  1.5-character gaps (a UART delivers in bursts, a USB adapter in slices of
  its latency timer, which the framework asks to be low at open).
- The 3.5-character silence is kept as the least gap before the next
  request, measured from the last byte received.
- After a timeout or a broken answer the worker discards input and waits a
  quiet time (the 3.5-character silence, at least 20 ms) before the next
  request to any device on the line: a late answer has no transaction id on
  RTU and would otherwise be read as the answer to the next request.
- The port is opened exclusively. A port that disappears (an error or end
  of file on read) is closed at once and reopened by its configured path
  (`device`, CS; it allows stable names such as `/dev/serial/by-id/...`), so that a
  replugged USB adapter is found again. Before close, pending output is
  flushed (`tcflush`), so that `close` does not wait.
- A bit rate the port refuses is a connect error with its own diagnostic,
  never a silent nearest rate.
- A pseudo-terminal has no bit rate, no parity errors and no RS-485 mode:
  tests against the RTU simulator check framing and gaps between requests
  only. Serial timing is checked on the target board (#41).

### 7.3 TCP connections

Decision D-95. **Review by a person** (the first two points are command
safety).

- **A write that times out closes the connection.** On `tcp` and
  `rtu_over_tcp`, a write whose deadline passes without a complete answer
  closes the connection abortively (`SO_LINGER` 0, so that unsent and
  unacknowledged bytes are discarded) before the result is posted. Without
  it the kernel retransmits the frame for minutes, and the device may
  operate long after the command was answered TIMEOUT and expired. TIMEOUT
  with `NO_RESPONSE` stays right: bytes already acknowledged cannot be
  recalled.
- **Consecutive timeouts are a lost connection.** On a TCP channel, n
  consecutive requests that timed out with no valid answer from any device
  between them (`lost_after_timeouts`, CS, default 3) close the
  connection abortively and reconnect.
- The socket's user timeout (`TCP_USER_TIMEOUT`) is set to the largest
  request deadline of the channel, so that the kernel gives up when the
  driver does.
- What remains: a serial server or gateway behind `rtu_over_tcp` buffers
  on its own, and may execute a write after our timeout. "Never executed
  late" holds up to our last byte. CS warns when a device's `timeout` is
  below the gateway's own forwarding time, where configured.
- **Idle close.** While idle the worker waits on its socket too
  (`POLLRDHUP`) and reconnects when the peer has closed. Before a write it
  checks the socket once more. A write that cannot be sent because the
  connection is gone, before any byte left, is REJECTED with `COMM_FAIL`
  (nothing sent): the requester may repeat it.
- **Back-off.** A lost connection fails every device of the channel at
  once (`CONNECTING`, COMM_FAIL), without spending retries. The worker
  reconnects with a back-off from `reconnect.min` doubling to
  `reconnect.max` (CS), reset after the first valid answer, not after the
  connect: a device that accepts and closes at once must not be retried
  every second.
- **Addresses.** Phase 2 accepts literal IPv4 and IPv6 addresses only (CS);
  a host name would mean a resolver call on the worker, which has no
  deadline the driver can enforce and allocates. Connect has its own
  deadline, `connect_timeout` (CS).
- The bytes of one answer are bounded (7.1).

### 7.4 Demotion and recovery

- A device is demoted after `demote_after` consecutive failed requests
  (CS), counted across poll groups. A request is one failure after its
  retries, not one per attempt.
- What counts: a timeout, a broken answer, a gateway that cannot reach its
  device (annex A). What does not: busy, `NO_VALUE`, `ABSENT`, and any
  failure within `startup_grace` after connect or a detected restart.
- Devices that share a unit id (3.4) share one failure count: a dead unit
  costs `demote_after` requests once, not once per slice.
- A demoted device: tags BAD with COMM_FAIL; no polls; one probe request
  every `backoff`, doubling to `backoff_max` (CS); a probe that succeeds recovers it to
  `INITIALISING`, then `RUNNING`.
- **Probe budget.** Probes are sent only in time no due request needs, at
  most one probe in flight per channel, with the device's timeout but at
  most 1 s, so that 200 dead devices behind one gateway at night do not
  starve the live ones (CS may lower the bound).

Phase 2.

### 7.5 Staleness

**open: needs the product owner's ruling (X-11).** Reference 6.2: "If a
point is not refreshed within its `max_age`, its tag gets the `STALE`
flag." This draft reads "refreshed" as "confirmed".

- A polled point is STALE when no read of it has succeeded within its
  `max_age` (CS). STALE is added to its last value and quality, once, and
  cleared by the next good read. It is checked at every transaction
  boundary, so a healthy device's points are not marked late by their
  neighbours' timeouts.
- Room: a reported point is STALE when, within `max_age`, there was no
  report of it, no integrity answer that includes it, and no link
  supervision that proves the session alive.

Committee recommendation: accept.

### 7.6 Device restart

A device's restart is detected from a configured indication (CS
`restart_indication`: a run-time counter going backwards, a power-up flag, an
identity change). The device goes back to `INITIALISING`: identity check,
points NOT_INIT until read, selections end, refreshes stop. Re-sending
device-side settings is room. Phase 2.

## 8. Write requests

**Review by a person:** this whole section is command handling, select
before operate and fail-safe behaviour.

Nothing here relaxes TD: requests are delivered at most once; requests still
queued at attach are answered ABORTED by the library; expired and
over-long requests are answered EXPIRED by the library and never reach the
driver; every request needs its event record accepted before it is posted
(TD 9.2, 9.3). The requester posts that record; neither the library nor
the driver can check the pairing. The one place where this contract
qualifies "no audit, no action" is a fail-safe action (8.13), and it is an
open ruling.

### 8.1 Requests and orders

The command thread takes a `WriteRequest` (TD 9.4) and, once the checks of
8.2 that need no device pass, hands the worker an order: the request, the
generation it was taken in, the output's compiled configuration, and the
action. Operations: `DIRECT` writes; `SELECT`, `OPERATE` and `CANCEL` as in
8.6; `READ_NOW` as in 8.14.

### 8.2 Checks, in order

The first check that fails answers the request and stops it; nothing is
sent to the device. Every check runs for every source.

| # | Check | Thread | Fails with |
| --- | --- | --- | --- |
| 1 | The library's: not expired, lifetime within 60 s, the record valid (TD 9.2, 9.4) | Library | EXPIRED, or passed over |
| 2 | The target is a point of this driver with an output, or a readable point for `READ_NOW` | Command | REJECTED, `UNKNOWN_TARGET` |
| 3 | The operation is allowed for the output's kind (8.3) and control model: `SELECT`, `OPERATE`, `CANCEL` only on an `sbo` output; `DIRECT` from OPERATOR only on a direct output; `DIRECT` from AUTO or REMOTE on either | Command | REJECTED, `WRONG_CONTROL_MODEL` |
| 4 | The value: of the tag's type; finite (NaN and infinity are refused before any limit is compared); one of the kind's values (8.3); within the output's limits (CS 7.2); convertible to the raw type (8.4) | Command | REJECTED, `INVALID_VALUE` |
| 5 | Source against mode (reference 8.1, below) | Command | NOT_AUTHORISED |
| 6 | For AUTO, on an output whose area is `local_blocks_auto` (CS 7.1): the area's local-panel tag reads GOOD and false | Command | NOT_AUTHORISED, `LOCAL_AUTHORITY` |
| 7 | Selection rules (8.6) | Command | REJECTED, `NOT_SELECTED`, `ALREADY_SELECTED`, `SELECTION_MISMATCH`, `SELECTED` |
| 8 | Device state (3.5): `DISABLED`, `MISMATCH`, `DEMOTED`, `REFUSING` | Command | REJECTED, `CONFIG_MISMATCH` (disabled, mismatch) or `DEVICE_UNAVAILABLE` (demoted, refusing) |
| 9 | Write rate limits (8.9) | Command | REJECTED, `RATE_LIMITED` |
| 10 | The order queue to the worker has room | Command | REJECTED, `BUSY` |
| 11 | When the worker starts the order: the connection is up; `expiry_mono_ns` checked on the worker's clock right before the first byte (TD 9.2) | Worker | REJECTED, `COMM_FAIL` (nothing sent); EXPIRED |

**Check 5, source against mode** (decision D-92). The mode tag of the
output (owned by `scada-cmd`; coding 1 AUTO, 2 MANUAL, 3 REMOTE, agreed with
CS) is read at the check.

- OPERATOR: only while the mode reads MANUAL with GOOD quality.
- REMOTE: only while it reads REMOTE with GOOD quality.
- AUTO: while it reads AUTO with GOOD quality; and, **open: needs the
  product owner's ruling (O-2)**, while it reads BAD with OWNER_DOWN as its
  only flag and a stored value of AUTO. Only `scada-cmd` changes the mode,
  so while it is down the stored mode cannot change, and automatic control
  continues. Reference 14.4 keeps `scada-cmd` out of the watchdog chain, so
  the reference does not make control depend on it. NOT_INIT, any other
  flag, a value that is not 1, 2 or 3, and a read that is not `SCADA_OK`
  fail the check for every source (fail closed). Committee recommendation:
  accept.
- In phase 2, before `scada-cmd` exists, tests own the mode tag.

`scada-cmd` checks value limits, interlocks, authority and mode itself
(reference 8.2). The driver checks again because AUTO and REMOTE requests do
not pass through `scada-cmd` (reference 8.4), and because the driver is the
last place before the device. The driver does not evaluate interlocks: they
are in the logic for AUTO (reference 8.4) and in `scada-cmd` for OPERATOR.

### 8.3 Command kinds

The output's `kind` (CS 7.2) decides what a request may carry and what is
sent. Decision D-99. CS cites this table.

| Kind | Request value | What is sent | Phase 2 |
| --- | --- | --- | --- |
| `single` | BOOL 0 or 1 | The address's `off` or `on` encoding, or the value converted (8.4) | Yes |
| `double` | INT64 1 (open, off) or 2 (closed, on); 0 and 3 refused | The `off` or `on` encoding | Yes |
| `pulse` | BOOL 1 only | The `on` encoding, once. The device makes the pulse (a coil the device resets itself, a register that triggers a timed output) | Yes, device-timed only |
| `trigger` | BOOL 1 only | The configured value, once (a reset, a setting-group switch, an acknowledge). `on_only` | Yes |
| `setpoint` | The tag's type | The value converted to raw (8.4), with the address's write encoding | Yes |
| `step` | INT64 +1 or -1 | The `on` (raise) or `off` (lower) encoding, once | Yes |
| `latch` | BOOL 0 or 1 | The `off` or `on` encoding; the device holds the state until commanded otherwise | Yes |

- A **framework-timed pulse** (write on, wait, write off) is not in phase 2:
  an off write that times out would leave the output energised, and no
  write is retried (8.9). Room: if it is added, its trailing edge is the
  one write repeated until it succeeds or the output is reported failed
  with an alarm, and it runs as an exclusive sequence (6.6) timed by the
  worker at transaction boundaries, never by a blocking wait.
- `pulse` and `trigger` are never refreshed (8.12) and never a safe or
  release action (8.13).
- X-16: a setting-group switch is a `trigger`; settings edits are out of
  scope. **open: needs the product owner's ruling (X-16).** Committee
  recommendation: accept.

### 8.4 Engineering value to raw value

Decision D-94. **Review by a person.**

- A request carries the engineering value in the tag's type (TD 9.4). The
  raw value is the inverse of the point's scaling (5.1, step 4), computed
  in binary64 with contraction off:
  - `scale: { gain, offset }`: raw = (value − offset) / gain;
  - `scale: { divide_by, offset }`: raw = (value − offset) × divide_by;
  - then, for an integer raw type, rounded half away from zero; for
    `float32`, rounded to nearest, ties to even.
- The result is refused with `INVALID_VALUE` when it is not finite, or
  outside the raw type's range (for `float32`, when it overflows to
  infinity).
- A point with `enum`: the value must be a defined code, written as the
  code. A double point: by its `on` and `off` encodings.
- Not writable (refused at configure, CS code): a point with
  `exponent_from` (the exponent is the device's), `mod10k`, `datetime`,
  `epoch`, `string`, and a `bit` or `field` without a `write.mask` (the
  framework never reads, changes and writes back a word).
- `safe_state` and `limits.release` state their domain (CS 7.2): `{ raw:
  <pattern> }` is written as the pattern, without conversion; `{ value:
  <engineering> }` is converted like a request.
- Golden vectors (conformance test 5) cover the conversion: 12.5 kW with
  `gain: 0.1` on `int16` is raw 125; 75.0 % with `divide_by: 10` is raw
  750; a value that rounds outside `int16` is refused.

### 8.5 Write encodings

An output's encoding is an ordered list of protocol writes per value, from
the point's `address` (CS annex A.5): fixed values, separate addresses for
on and off, inverted polarity, a whole word with a mask, exactly the
quantity a device demands, a write type other than the read type. The
protocol compiles it at `configure` and refuses what it cannot send.

- A list is sent in order as an exclusive sequence (6.6). If a write of the
  list fails after an earlier one succeeded, the result is DEVICE_ERROR
  with reason `PARTIAL`, the rest is not sent, and the framework reads back
  every member that has a read-back point (8.10).
- The framework never reads a word, changes bits and writes it back, and
  never resends a stored image of a word.
- Writes of several tags in one request (a curve, a table) are not in
  phase 2: a request has one target (TD 9.4). Room: a write group as one
  output.

### 8.6 Select before operate

**open: needs the product owner's ruling (X-1 and X-7).** Committee
recommendation: accept both as drafted. **Review by a person.**

Reference 6.4: "Where a protocol has a native select-before-operate
procedure, the driver must use it for `SELECT` and `OPERATE` requests.
Where it does not (Modbus), the driver accepts `OPERATE` after a `SELECT`
held by the command gateway (section 8.2)." Native select is a property of
the device and point, declared by the protocol (X-1, option (b)). Phase 2
implements the gateway select; a native select over Modbus, where a device
has one, is room until its procedure is documented (annex A).

**Selections.** The command thread keeps one selection per output. It holds
the SELECT's request id, source, `user_id`, value, time, the mode it was
made in, and, for a native one, the channel's connection epoch (2.4).

**Rules:**

1. A SELECT on an output with a live selection is refused
   (`ALREADY_SELECTED`), whoever sends it.
2. An OPERATE or CANCEL must name, in `select_request_id`, the request id
   of the live selection of its target, with the same source and
   `user_id`; an OPERATE also the same value. Otherwise `NOT_SELECTED` (no
   live selection with that id) or `SELECTION_MISMATCH`.
3. A selection ends at the first of: its OPERATE (whatever the result); its
   CANCEL; its timeout; a change of the output's mode tag; a change of the
   channel's connection epoch while a native selection is outstanding
   (counted in `sys.dev.<device>.selections_lost`); a reload of the output;
   a generation change; a driver restart (TD 9.2).
4. A DIRECT on an output with a live selection is refused (`SELECTED`): an
   AUTO write must not act on an output an operator holds.
5. The driver times its selections itself (TD 17, item 19). The timeout is
   the output's `select_timeout` (CS, at most 60 s).
6. A CANCEL with no live selection is answered OK with `NOTHING_TO_CANCEL`;
   nothing is sent. A CANCEL of a live native selection, and its timeout,
   send the device's cancel when the protocol has one.
7. A native selection is shown in `<output>.device_selected` (BOOL, owned
   by the target's channel worker, generated by CS only where the protocol
   declares a native select; never in phase 2), so that `scada-cmd` sees a
   selection the device lost.

**Gateway select** (every point in phase 2): a SELECT that passes the
checks makes the selection and is answered OK, with no device I/O. Its
OPERATE writes the value as a DIRECT would.

**Native select** (room), per output `native_select` (CS 7.2), decision
D-89 (X-7):

- `at_select` (the default): SELECT makes the device's select at once; the
  device's checks answer at select time; OPERATE makes the device's
  operate; CANCEL and the timeout make the device's cancel. CS refuses a
  `select_timeout` above the device window.
- `at_operate`: the gateway holds the operator's selection, and the driver
  runs the device's select and operate back to back at OPERATE:
  1. SELECT: checks 2 to 10 run, the driver makes a gateway selection and
     answers OK. **Nothing on the device is verified** (not that it is
     reachable, not its local state, not its interlocks).
  2. OPERATE within `select_timeout`: checks 2 to 10, with rule 2.
  3. The worker starts it at the next boundary, before any due poll (6.2),
     with check 11.
  4. It sends the device's select; deadline the earlier of the expiry and
     the device's request deadline (7.1).
  5. On a positive answer at `t_sel` it checks the expiry and sends the
     operate at once, with no other request to that device between (6.6).
     If `t_sel` plus the device window would pass first, it sends the
     device's cancel and answers REJECTED with `SELECT_EXPIRED`.
  6. It waits for the operate's answer, to the earlier of the expiry and
     its deadline.
  7. Results: select refused, REJECTED `SELECT_FAILED` with the device's
     cause; select unanswered, the device's cancel and REJECTED
     `SELECT_FAILED` (no operate was sent); operate refused, REJECTED with
     the mapped reason; operate unanswered, TIMEOUT `NO_RESPONSE` (and 7.3);
     operate accepted, OK. The selection ends in every case.

  Steps 3 to 7 take at most one boundary wait, two requests and one step
  between them: about 2 s at a device timeout of 1 s, within the expiry
  `scada-cmd` sets. CS refuses an `at_operate` output whose device window
  is below twice the device's request deadline.
- A `DIRECT` from AUTO or REMOTE on a point whose device requires a select
  runs steps 3 to 7.

### 8.7 Expiry

- The library answers EXPIRED what expired before the driver takes it (TD
  9.2). The worker checks the expiry again right before the first byte
  (check 11) and before each write of a list. A list interrupted by expiry
  is DEVICE_ERROR `PARTIAL` when something was sent, EXPIRED when nothing
  was.
- A request deadline is never later than the request's expiry. A write
  whose answer has not arrived by its deadline is TIMEOUT `NO_RESPONSE`,
  and on TCP its connection is closed abortively first (7.3).
- A request is never executed after its expiry (up to our last byte, 7.3),
  never twice, and never after the driver detached from the generation it
  was taken in (9).

### 8.8 Results

One result per request taken, posted by the command thread (TD 9.2) at the
request's final outcome. A result the queue refuses (`SCADA_E_FULL`) is
posted again at each wake of the command thread until the request's expiry,
then counted in `sys.proc.<process>.answers_dropped`. Status values are the
seven of TD (frozen); the driver adds a reason and the raw protocol code
in `WriteResult.protocol_error` (decision D-85):

```
protocol_error = (reason << 16) | raw        // reason 0 .. 32767, raw 0 .. 65535
```

- `raw` is the protocol's own code (a Modbus exception code), or 0. A raw
  code wider than 16 bits is given as 0xFFFF here and in full in
  `sys.dev.<device>.last_status`.
- What the field means today: TD 9.4, `int32_t protocol_error; //
  driver-specific, 0 if none`; reference 5.3 lists "protocol error code".
  The library checks only `status` and the reserved fields (TD 14.1), and
  no consumer reads `protocol_error` before phase 5. The packing keeps the
  field driver-specific for the library and 0 when there is nothing to
  say. **open: needs the product owner's ruling (R-4)**: if "protocol error
  code" is read as the raw code alone, the alternative is the raw code here
  and the reason in a diagnostic tag. Committee recommendation: accept the
  packing.

| Reason | Value | Status | Meaning | Requester may retry |
| --- | --- | --- | --- | --- |
| (none) | 0 | OK | Accepted by the device | |
| `ALREADY_IN_POSITION` | 1 | OK | The device reports nothing to do | |
| `NOTHING_TO_CANCEL` | 2 | OK | CANCEL with no live selection | |
| `UNKNOWN_TARGET` | 10 | REJECTED | Not an output of this driver | No |
| `WRONG_CONTROL_MODEL` | 11 | REJECTED | Operation not allowed for the output, or refused by the device for its control model | No |
| `INVALID_VALUE` | 12 | REJECTED | Type, finiteness, kind, limits, raw range, or refused by the device | No |
| `NOT_SELECTED` | 13 | REJECTED | No live selection with that id | Select again |
| `ALREADY_SELECTED` | 14 | REJECTED | A live selection exists | No |
| `SELECTION_MISMATCH` | 15 | REJECTED | Source, user or value differ from the SELECT | No |
| `SELECT_FAILED` | 16 | REJECTED | The device refused or did not answer its select; no operate sent | No |
| `CONFIG_MISMATCH` | 17 | REJECTED | Device disabled or in `MISMATCH` | No |
| `RATE_LIMITED` | 18 | REJECTED | A write rate limit (8.9) | Later |
| `BUSY` | 19 | REJECTED | The order queue was full, or the device answered busy and did not execute | Yes |
| `REJECTED_INTERLOCK` | 20 | REJECTED | The device's interlock refused | No |
| `REJECTED_SYNCHROCHECK` | 21 | REJECTED | The device's synchrocheck refused | No |
| `REJECTED_LOCAL` | 22 | REJECTED | The device is in local control | No |
| `REJECTED_MODE` | 23 | REJECTED | A permanent state refusal of the device | No |
| `NOT_SUPPORTED` | 24 | REJECTED | The device does not have the function | No |
| `COMM_FAIL` | 25 | REJECTED | Not connected; nothing was sent | Yes |
| `SELECT_EXPIRED` | 26 | REJECTED | The device window passed before the operate (8.6); cancel sent | Give it again |
| `DEVICE_UNAVAILABLE` | 27 | REJECTED | Device demoted or refusing (check 8) | Later |
| `SELECTED` | 28 | REJECTED | DIRECT on an output with a live selection | No |
| `PARTIAL` | 30 | DEVICE_ERROR | Part of a list of writes was applied | Read the feedback first |
| `DEVICE_FAULT` | 31 | DEVICE_ERROR | Any other error answer | No |
| `NO_RESPONSE` | 40 | TIMEOUT | Sent, no answer: executed or not is unknown | Read the feedback first |
| `TIMEOUT_CONFIRM` | 41 | TIMEOUT | No confirmation of a two-phase write (room) | Read the feedback first |
| `TIMEOUT_TERMINATION` | 42 | TIMEOUT | Confirmed, no termination in time (room) | Read the feedback first |
| `EXPIRED_BEFORE_SEND` | 50 | EXPIRED | Check 11 | No |
| `NOT_STARTED` | 60 | ABORTED | Taken, not started, and the generation retired or the driver stopped (9) | No |
| `LOCAL_AUTHORITY` | 70 | NOT_AUTHORISED | Check 6 | No |
| (none) | 0 | NOT_AUTHORISED | Check 5 | No |

- "Requester may retry" is guidance for `scada-cmd` and `scada-control`;
  the driver never retries a write (8.9).
- The mapping from protocol codes to reasons is the protocol's, overridable
  per device type in configuration (CS).
- **Two-phase writes** (room). A protocol whose write is confirmed and
  later terminated completes once, at termination, within the output's
  `termination_timeout` (CS; default the request's expiry), so a two-phase
  command is bounded by the 60 s request lifetime (TD 14): a drive that
  travels longer ends as `TIMEOUT_TERMINATION` although it works. Checked
  against reference 8.2: `scada-cmd` shows EXECUTING from its post to the
  result; a refusal at confirmation arrives as fast as a one-phase one; its
  `feedback_timeout` starts at the result. A second result per request
  would be dropped by the requester as a duplicate id (TD 9.2), so it would
  be a change to TD. The stage is published for people in
  `sys.dev.<device>.last_command_stage`; a per-output stage tag is room.
- Static protocol options of a command (a qualifier, check bits, an
  originator) come from the target point's `address` (CS annex A.5). The
  frozen `WriteRequest` needs no room for them.

### 8.9 No retry, rate limits, storage

**Review by a person.**

- The framework never resends a write once any byte of it was sent, for any
  source (reference 14.4: "no automatic retry of operator commands"; for AUTO
  the next cycle of `scada-control` decides). A write that timed out is
  TIMEOUT `NO_RESPONSE`. A trigger and a pulse are sent once.
- A device's "busy, not executed" answer to a write is REJECTED `BUSY`; the
  requester decides. `busy_retries` (CS) applies to reads only.
- Rate limits (CS `write_limits`: `min_interval`, `max_per_day`, on any writable point, required with `storage: nvm`) are checked
  at check 9 and counted when the first byte of the write is sent, so a
  request refused later uses nothing. Nothing bypasses them but a reload
  with a new limit. Phase 2 keeps the counts in memory: a driver restart
  starts a new day count, so n restarts allow n times the daily writes.
  Durable counts are room.
- `storage` is required on every writable point (CS); `nvm` points have no
  cyclic source (no `refresh`, no control block, CS).

### 8.10 Accepted is not done

- OK means the device accepted the write by its protocol, nothing more.
  Devices accept writes they ignore (a command bit that acts only through
  site logic, a value outside a range the device discards, a reset ignored
  in a locked mode). Whether the plant followed is decided by `scada-cmd`
  from the feedback within `feedback_timeout` (reference 8.2, step 10). The
  driver never reports a command as having reached its feedback.
- Read-back: after an OK, PARTIAL or TIMEOUT write, when the output's
  feedback point is on the same channel, the framework schedules a
  `READ_NOW` of it after its `settle` time (CS), and after an edge command,
  so that the feedback is fresh when `scada-cmd` compares it. A feedback on
  another channel or driver is read by its own poll. The read-back is not
  part of the result.
- A write-only point with a tag and no read-back: the framework writes the
  value last written with UNCERTAIN and LOCAL_TIME, the source time the time
  of the OK. UNCERTAIN marks "written, not confirmed". A `wo` point is never
  polled.

### 8.11 Other masters

Another master's commands show only as changes of the feedback, which the
driver reports like any change. A refreshed setpoint (8.12) overwrites
another master's value at every period: the template says so where a
device has a second setpoint source.

### 8.12 Refresh of setpoints

**open: needs the product owner's ruling (X-5).** **Review by a person.**
Phase 5; room in phase 2. Decision D-90.

Some devices drop a setpoint after a validity time, or fall back when
setpoints stop. This draft proposes option (a):

- Only a `setpoint` output may have `refresh` (CS); never one that is
  `dangerous`, `sbo` or `nvm` (CS refuses them).
- The framework writes again the value of the last request executed with
  OK, every period, as part of that request: its event record covers the
  refreshes, and says that the output refreshes (the journal schema of phase
  5). No new record is posted per refresh.
- A refresh is made only while all hold, checked before each: the mode tag
  reads the mode of that request's source, GOOD (as check 5); the request
  tag of that mode (reference 8.1) reads GOOD, not STALE, not OWNER_DOWN,
  and holds the refreshed value; the generation is the request's; the
  driver has not restarted; the device is usable.
- When one fails, refreshing ends. The end is visible: `<output>.refresh`
  (INT64, owned by the target's channel worker, CS 12.5: 0 none, 1 active,
  2 ended other than by a new request) is set to 2, and the main thread posts a best-effort
  event record of the end (8.13 rules). An alarm on value 2 is phase 7's.
  The device's own fallback then acts (reference 14.4).
- What ends a refresh unplanned: a restart of `scada-cmd` (its request tags
  read OWNER_DOWN), any tag-set commit (a generation change), a new request
  the driver refuses (the request tag no longer holds the refreshed value).
  For a curtailment limit ordered by a grid operator, the device then falls
  back after its own time.
- Who re-establishes a standing MANUAL setpoint after such an end: in this
  draft nobody automatically; the operator gives it again, alerted by the
  tag. The alternative, `scada-cmd` posting it again from the retained
  manual request tag, needs a journalled request with a system user id
  (TD 9.4 requires a non-zero `user_id` for OPERATOR) and is for the
  owner.
- Refreshes obey rate limits and pacing, are never retried, and never
  select. A failed refresh is counted (`sys.dev.<device>.refresh_failures`)
  and the next period tries again.
- For AUTO, `scada-control` already posts again "whose refresh interval
  expired" (reference 7.2, step 3); an AUTO output uses one of the two (CS).

Committee recommendation: accept (a), with the end made visible.

### 8.13 Fail-safe behaviour

**Review by a person.** Decision D-91.

One table for every case. The keys and their defaults are CS's (7.4):
`on_shutdown: hold | safe | release`, `on_control_loss: { action, after }`
and `on_generation_change: hold | safe`; the defaults are restated below and
CS wins if they differ. This section defines the behaviour.

| Case | Key | Delay | Default, local output (`scada-io`) | Default, remote output (driver) | Who acts |
| --- | --- | --- | --- | --- | --- |
| Orderly stop of the driver or `scada-io` | `on_shutdown` | None | `safe` (reference 6.5: "on shutdown") | `hold` (reference 14.4: "hold (default for most process outputs)") | The stopping process, before it disconnects |
| `scada-control` down while the output is in AUTO | `on_control_loss` | `after` (local default 0: at its heartbeat timeout) | `safe` (reference 6.5: "when the control runtime's heartbeat stops") | `hold` (reference 14.4: "hold, or go to safe state after a timeout") | The command thread (remote), the scan thread (local) |
| `scada-cmd` down while the output is in MANUAL | None | | `hold` | `hold`; a refreshed setpoint stops refreshing and the device falls back (8.12) | The worker |
| Generation change | `on_generation_change` (reference 19.2) | The restart | `hold`; `safe` when the limit of reference 5.5 is exceeded | The same | `scada-conf` decides; the driver acts (9.2) |
| Connection to the device lost | None | | n/a | Nothing can be written; the device's own fallback (reference 14.4) | The device |
| `scada-io` crashes | `on_shutdown` | None | As the hardware leaves the line, then `safe` by the stop step (13.4) | n/a | The service manager's stop step |
| The whole system hangs | None | | Hardware default (reference 14.4) | The device's fallback | Hardware watchdog |

- `safe` writes the output's `safe_state`; `release` writes `limits.release`
  (both raw or engineering as declared, 8.4); `hold` writes nothing.
- A safe or release write is a DIRECT write, sent once, never retried,
  bounded by the stop time of the service manager.
- CS refuses `safe` and `release` on a `pulse`, `trigger` or `dangerous`
  output unless the output names its safe action explicitly.
- After a driver restart or a generation change no output is written until
  a request of the new generation arrives (reference 14.4), except a local
  output's `at_start` (13.4).

**A fail-safe action never waits for an audit record.** **open: needs the
product owner's ruling (R-5)**, because it qualifies "no audit, no action"
for fail-safe actions only. The action is taken; then an event record is
posted best-effort by the main thread, the one producer of the process's
events queue (`events.drv-<protocol>`, or `scada-io`'s, CS 12.3; from phase
5), to which the command thread and the workers hand their records
in-process; a refused post is counted (`sys.proc.<process>.safe_unjournalled`) and
logged. Reason: a safe state on loss of control is the configured answer to
a failure, and the audit service being down is likely part of the same
failure; reference 6.5 requires `scada-io` to apply it "when the control
runtime's heartbeat stops", and the phase 5 acceptance requires that
"outputs reach safe state when `scada-control` is killed". "No audit, no
action" stays whole for commands. Committee recommendation: accept, for
local and remote outputs alike.

### 8.14 `READ_NOW`

Reads the target's point at the head of the channel's orders. It changes
nothing on the device: refused (`NOT_SUPPORTED`) for a point in a
`side_effect` or `exclusive` range or a `wo` point. Answered OK when the
device answered (outcomes `VALUE`, `NO_VALUE`, `ABSENT`, `DEVICE_INVALID`;
the tag says which), TIMEOUT when it was `COMM_FAIL`. It needs no mode check
and no selection; it is a request and needs its event record (TD 9.3).
Phase 2.

## 9. Generation change and restart

This section answers #84. **Review by a person** (restart behaviour).

### 9.1 After every attach

- Before a worker's first request, the framework writes every tag of each
  of its owners: value as stored, quality BAD, flag NOT_INIT,
  `source_time_ns` 0, except a tag restored from a retain file (RESTORED),
  which keeps its restored value until the device is read (TD 6.3).
  Diagnostic tags are written with their current values.
- Handles are resolved again.
- No output is written until a request of this generation asks for it
  (8.13).

### 9.2 Duties in a RETIRING generation

When the main thread learns that the generation is RETIRING
(`scada_tag_heartbeat` or `scada_tag_status` returns `SCADA_E_RETIRING`):

1. **New requests.** The command thread calls `scada_queue_next` on every
   `cmd` queue until it returns `SCADA_E_RETIRING`; the library answers
   ABORTED what is still queued (TD 9.5). Nothing new is dispatched.
2. **Taken and not started.** An order a worker has not started is returned
   unstarted and answered ABORTED with `NOT_STARTED`: it was not executed,
   and results are accepted in a RETIRING generation (TD 9.5).
3. **In flight.** An order the worker has started completes and is answered
   before detach, bounded by its deadline. A read in flight is abandoned at
   once (its points keep their values; on a serial line the worker then
   waits the quiet time, 7.2). A write in flight is never abandoned. A
   two-phase order (room) whose termination has not come when the drain
   budget (1 s, TD 11.3) ends is answered TIMEOUT `TIMEOUT_TERMINATION`.
   The driver never acts on a request after it detached: otherwise the
   operator gives the command again in the new generation and the output
   operates twice.
4. **Selections** end; a live native one gets the device's cancel when the
   device is connected.
5. **Refreshes** end (8.12).
6. The main thread pauses the workers at their next boundary and detaches,
   outside every cycle. A real-time thread never calls `scada_queue_next`
   (2.4).
7. **No answer across the change.** Every outcome carries the generation of
   its order. After the reattach the command thread drops, logs and counts
   (`sys.proc.<process>.stale_outcomes`) any outcome of an older generation;
   ids restart at 0 and could name a request of the new one.
8. **Reattach:** attach to `current`, load the expanded configuration of
   the new generation (B-12), resolve, the writes of 9.1, resume.
   Connections stay open unless the change altered the channel.
9. **Outputs** hold, or go to their configured behaviour (8.13), until they
   are written again in the new generation: AUTO values by `scada-control`,
   operator commands by `scada-cmd` (reference 5.5; #84).

Steps 1 to 7 fit in the 1 s drain of TD 11.3 when every write deadline is at
most 1 s; a longer write delays the detach by its excess, and `scada-conf`
applies the limit of reference 5.5. Phase 2.

### 9.3 Driver restart

A driver that stopped has lost every selection and every order it held; the
library answers ABORTED what is still queued at its next attach (TD 9.2). A
request it took and did not answer is reported by its requester as failed
with its outcome unknown (TD 9.5). After the restart, 9.1 applies.

## 10. Process start, configuration and stop

- **Configuration.** A driver loads the expanded configuration of the
  generation it attached to, as CS 13 defines its delivery (17, B-12): the file
  carries the `config_hash` of its tag set, and the driver refuses to run
  on a file whose hash differs from the generation's header. It publishes
  the `config_id` it loaded in `sys.proc.<process>.config_id` and the
  outcome in `sys.proc.<process>.config_state`, so that `scada-conf` can
  see that an apply finished (reference 15.2: 2 s). It never expands
  templates itself.
- **No entry.** A driver process that starts and finds no entry for itself
  in the expanded configuration (no channel of its protocol) logs it once
  and exits with status 0, so that it does not run into the restart limit
  (CS 13). Who starts a driver whose first channel is committed is
  CS's (B-12).
- **Stop.** On a stop signal the main thread stops the command thread
  (requests still queued stay for the library to drain at the next attach,
  TD 9.2), applies `on_shutdown` (8.13), pauses the workers at their next
  boundary (a read in flight is abandoned, a write never), disconnects,
  detaches and exits, within a stop budget below the service manager's stop
  time (the unit sets both, #105).

## 11. Diagnostics and logging

### 11.1 System tags

The framework writes them; a protocol only reports counts and codes. Names
and types are generated by CS 12.5 by the rule below. Counters are UINT64
since attach; times INT64 ns UTC. States are written when they change;
counters, times and averages at most once a second (300 devices at one
request a second would otherwise put about 1,000 diagnostic changes a
second on the change rings). At about 22 tags per device, 300 devices take
about 6,600 tags; CS counts them against `limits.max_tags`.

Per process, `sys.proc.<process>.<signal>`, owner the main thread:

| Signal | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `realtime` | BOOL | | The process runs a real-time thread on the real-time tier (2.5) |
| `config_id` | UINT64 | | The first 64 bits of the `config_id` loaded (10) |
| `config_state` | INT64 | | 0 loading, 1 applied, 2 refused (10, 12) |
| `stale_outcomes` | UINT64 | count | Outcomes of an older generation dropped (9.2) |
| `answers_dropped` | UINT64 | count | Results not posted by the request's expiry (8.8) |
| `safe_unjournalled` | UINT64 | count | Fail-safe actions whose event record was refused (8.13; phase 5) |
| `log_dropped` | UINT64 | count | Log records the workers' log queues dropped (11.3) |

Per channel, `sys.drv.<channel>.<signal>`, owner the channel's worker:

| Signal | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `state` | INT64 | | 0 disconnected, 1 connecting, 2 connected |
| `connects` | UINT64 | count | Successful connects |
| `refused_connects` | UINT64 | count | Connections refused by the peer (3.5) |
| `secured` | BOOL | | The connection is authenticated and encrypted; false for every phase 2 channel (reference 13.3) |
| `period.<group>` | FLOAT64 | ms | Achieved period of each poll group, over its last 10 periods |
| `requests_per_s` | FLOAT64 | 1/s | Requests over the last 10 s |

Per device, `sys.dev.<device>.<signal>`, owner its channel's worker:

| Signal | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `state` | INT64 | | 3.5 |
| `last_ok_time` | INT64 | ns UTC | Last valid answer |
| `requests` | UINT64 | count | Requests sent |
| `errors` | UINT64 | count | Requests answered with an error |
| `timeouts` | UINT64 | count | Requests without an answer |
| `response_ms` | FLOAT64 | ms | Average time to the first byte over the last 100 requests |
| `requests_per_scan` | UINT64 | count | Planned requests of one period of each poll group (6.3) |
| `pacing_limited` | BOOL | | A group misses its period because of pacing (6.4) |
| `last_status` | INT64 | | Last protocol error or exception code, raw |
| `exceptions` | UINT64 | count | Error answers with a protocol code |
| `refusals` | UINT64 | count | Busy, gateway and not-started answers |
| `sentinels` | UINT64 | count | `NO_VALUE` samples |
| `invalid` | UINT64 | count | `DEVICE_INVALID` samples, and non-finite values without a marker |
| `points_absent` | UINT64 | count | Points currently `ABSENT` |
| `identity` | STRING | | Identity read (3.6), when the device has `identify` |
| `identity_ok` | BOOL | | Identity matched, likewise |
| `restarts` | UINT64 | count | Device restarts detected (7.6) |
| `selections_lost` | UINT64 | count | Native selections lost (8.6) |
| `last_command_stage` | INT64 | | 1 sent, 2 accepted, 3 completed (8.8) |
| `refresh_failures` | UINT64 | count | Phase 5, devices with a refreshed output (8.12) |

The device's "connection state, last successful poll time, actual poll
period per group, request count, error count, timeout count and average
response time" of reference 6.3 are `state`, `last_ok_time`,
`period.<group>`, `requests`, `errors`, `timeouts` and `response_ms`.

### 11.2 What is not a tag

The raw quality of a protocol (room, #119), the plan's request ranges (the
log at configure), counts per exception code (the log).

### 11.3 Logging

- Workers and the command thread never write to the journal: a send to the
  journal blocks when the journal daemon is slow, and a blocked worker is a
  stall (2.6). They push fixed-size records into a lock-free bounded queue
  per thread, drained by the main thread, which writes them. A full queue
  drops the record and counts it (`log_dropped`). The push allocates
  nothing and makes no system call; it is the only logging path on a
  worker, and it is allowed in `SCADA_RT` functions of a real-time thread.
- Each record has a fixed `MESSAGE_ID`, a level and named fields: device
  demoted, recovered, connection lost, connection refused, identity
  mismatch, point absent (summary per connection), stall (with the channel,
  surviving the restart in the log), deadline overrun, reload refused,
  configuration loaded, safe action taken, safe action unjournalled.
- Rate limit, the pattern of TD 10.1: the first record at once, then at
  most one summary per 10 s with a count, per `MESSAGE_ID` and device. A
  device that flaps every back-off period writes two records per period at
  most; absent points give one summary per connection, not one per point.

## 12. Reload

A commit reaches the driver as a reload for changes that keep the tag set,
or as a generation change for changes that alter it (reference 5.5, 12.4).
The mechanism (a signal through the service manager or a socket, and the
report back) is CS's (B-12).

- The main thread runs `configure` and `plan` on the new configuration
  aside, builds the new `CompiledConfig`, and swaps it as in 3.2. A refusal
  leaves the running configuration in force and is logged at ERROR; since
  `scada-conf` ran the same `configure` at validation, it is a fault.
- Reload completes within 2 s (reference 15.2, conformance test 31).

| Change | Applied | Effect |
| --- | --- | --- |
| Poll period or priority; device timeout, retries, pacing, keep-alive; scaling; markers; `max_age`; read rules | Live | At the next boundary; selections kept |
| A device's addresses, encoding or identity (a site map that keeps the tag set) | Live, that device | It goes `INITIALISING`; its points NOT_INIT until read; its selections end |
| An output's encoding, limits, select model, timeout, refresh, rate limits, fail-safe keys | Live, that output | Its selection and refresh end |
| A channel's address, port, serial settings | Live, that channel | It reconnects; its selections end |
| A device or channel enabled or disabled | Live, that device or channel | Disabled: selections end, it disconnects, its points are written BAD with NOT_INIT and `state` 0; enabled: `CONNECTING`. Its tags exist either way |
| Points, devices or channels added or removed; a tag type changed | Generation change | Section 9 |
| The protocol's code or the framework's version | Process restart | 9.3 |

## 13. Local I/O (`scada-io`)

Decision D-97. `scada-io` uses the same framework for threads, tags,
commands, diagnostics, reload and the generation change; its hardware
interface is the HAL of reference 6.5, not a frame protocol.

### 13.1 Backends and threads

- Backends: `simulator`, `gpio` (the GPIO character device), `iio`
  (industrial I/O), compiled in and chosen per channel (D-81). Keys are
  CS's (annex B, `io`): GPIO lines by chip label and line name or offset,
  never by `gpiochipN`; IIO channels by device name and channel name, never
  by `iio:deviceN`; no paths in configuration. The backend resolves names
  at start.
- The HAL has four calls (reference 6.5): `describe_channels` (main
  thread, at configure), `read_inputs` and `write_outputs` (the scan
  thread), `set_safe_state` (the scan thread, and the stop step of 13.4).
- Threads: main (owner `io`, CS 12.3), command (consumes `cmd.io.*`),
  one scan thread (owner `io.scan`), real-time under `SCHED_FIFO` with locked
  memory when the tier is available (reference 4.3), at the fastest control
  task period (default 10 ms).
- A backend declares per channel whether its calls are bounded. A GPIO line
  on a memory-mapped controller is; a line on an I2C or SPI expander is not
  (it sleeps on the bus, with the adapter's timeout); an IIO single read is
  not, unless the backend declares the converter fast. Unbounded channels
  are read by a helper thread at their own period, which hands samples to
  the scan thread through the bounded queue of 2.7. The scan thread touches
  only bounded channels.
- Phase 2 reads and writes single values: IIO inputs by single reads, IIO
  analog outputs by single writes. Buffered capture is room.
- Digital inputs: debounce (CS), in the kernel where the controller
  supports it, else in the backend's decode. Analog: scaling, clamping and
  a filter time constant (reference 6.5), in the decode.

### 13.2 Commands

Requests reach the scan thread as orders (2.4) and are written by
`write_outputs` at the next cycle; the checks of 8.2 apply, with check 8
reading the backend's channel state. A local output is never refreshed.

### 13.3 System calls of the scan thread

While the channels are open and inside a cycle, the scan thread makes only:

| Call | Why | Bound |
| --- | --- | --- |
| GPIO line request `ioctl`s: get values, set values | `read_inputs`, `write_outputs`, `set_safe_state` | Memory-mapped controller: microseconds; other controllers are not on this thread (13.1) |
| `pread` of an IIO attribute of a fast converter, `pwrite` of an IIO output | Single values | Declared per channel by the backend |
| `read` of the order eventfd | Clear the order signal | Non-blocking |
| `clock_nanosleep` to the next cycle | The cycle | The period |
| `clock_gettime` | Times | vDSO; a system call only on the clock sources of TD 13.1, which reports it |
| `scada_tag_notify` (futex wake; once a second a look, `fstatat` and possibly `open`, `mmap`, a populate; once a minute the clock check) | Wake consumers | Not real-time safe (TD 13); open, depends on #116 point 3 (2.5) |

Opening lines and IIO attributes, reload and detach run outside the cycle.

### 13.4 Outputs at start, stop and crash

- **Start.** A GPIO line requested as an output must be given a level in
  the request. Each output has `at_start: hold | safe` (CS): `hold` reads
  the level found and requests the line with it; `safe` requests it at its
  safe state. Reference 14.4's "outputs are not driven until the control
  runtime has run one full cycle" is met by `hold`: the level is not
  changed until a request arrives.
- **Loss of `scada-control`.** After `on_control_loss.after` an output in
  AUTO goes to its `on_control_loss` action (8.13; default `safe`), applied
  unconditionally, journalled best-effort. The safe state is the output's
  `safe_state` (CS 7.2), applied without `scada-cmd` running.
- **Orderly stop.** `on_shutdown` (default `safe`, 8.13), then the lines
  are released.
- **Crash.** The kernel releases a GPIO line when its descriptor closes and
  promises no level afterwards; most controllers keep the last level, some
  do not. The process that died cannot apply a safe state. The install tree
  provides a small stop step (`scada-io-safe`), run by the service manager
  after every stop of `scada-io`, crash included: it requests the lines of
  outputs whose `on_shutdown` is `safe`, drives their safe states, and
  exits. A hung system is the hardware watchdog's, and outputs go to the
  hardware default (reference 14.4).

Phase 2 (#107).

## 14. Simulator requirements

Every driver ships with a simulator of its protocol (`CLAUDE.md`). A
simulator is a separate process that serves simulated devices over the real
transport (a TCP port, a pseudo-terminal pair) and is controlled by the test
through a control socket, never by the driver under test.

Every simulator must be able to:

1. Serve at least 300 devices across its instances, on one or many
   connections, each with a configured map and values that follow a pattern
   or are set by the test.
2. Answer late, not answer, answer with each of the protocol's error
   answers, send a broken frame, a bad CRC, an answer with another
   transaction id or from another unit, more bytes than the largest legal
   frame; close the connection; refuse a connection; open a connection and
   then answer nothing; stay silent after a power-up.
3. Stop reading (not close) for a set time, then resume (7.3, test 24).
4. Enforce pacing: a minimum interval between requests, a limit of values
   per request.
5. Close an idle connection after a set time; limit its connections.
6. Serve gaps as "illegal address", as zeros or as typed sentinels; refuse a
   read that splits a value; refuse a write of the wrong quantity.
7. Treat ranges as side-effect reads (clear on read, per client), and points
   as write-only.
8. For writes: apply; ignore; clamp; refuse with a chosen error; answer
   busy; leave unanswered; apply part of a list; apply after a delay visible
   in a read-back elsewhere. Count writes per point.
9. A second master acting on the same device.
10. Change its identity; signal a restart.
11. Report every request received, in order, with its time.

Phase 2: the Modbus TCP and RTU simulators (#105, #106) provide 1 to 11
with the Modbus specifics of annex A (exception codes, unit-id routing
with a gateway's offline and cached states, function refusal per device).
A pseudo-terminal has no bit rate, parity errors or RS-485 mode: the RTU
simulator checks framing and the gaps between requests it can measure, and
serial timing is tested on the target board (#41). The `scada-io` simulator
backend (#107) provides 1, 2 (silence) and 8 (apply, ignore) for its HAL
calls, and reports outputs and safe states.

## 15. Conformance tests

Protocol-independent. `libscada-driver` passes them with a test protocol (a
scripted frame protocol over a loopback transport, #104); each phase 2
driver passes them against its simulator (#105 to #107), with the tests of
its own issue. On x86-64 and ARM64. Tests wait on conditions with timeouts,
never on sleeps; timing tests assert order and lower bounds, never upper
bounds on loaded runners (except the timed reload, test 31, with its 2 s).

Interface, threads, real-time

1. Every call runs on its thread (a debug check in the test protocol).
2. A worker that stops moving its counter ends the process with its exit
   status within its stall limit plus 1 s (2.6).
3. Allocation test of 2.5 on the `scada-io` scan thread and on a test
   protocol worker: 0 allocations per thread (malloc and `operator new`) in
   10,000 cycles with their error paths. Not under sanitizers; native
   runners only.

Samples

4. Each outcome of 5.2 gives its quality and flags; only `COMM_FAIL` counts
   toward demotion.
5. Golden vectors (5.1, 8.4): raw bytes and a binding give value bits,
   quality and flags, bit-identical on both architectures: markers on the
   pattern after word order (one vector with `word_order: little`),
   non-finite without marker, exponent (negative, invalid), scale (integer,
   overflow, decimal), `raw_range`, `on_invalid` actions, enumerations
   (unknown code), double points (codes 0 to 3, a `states` map), counters
   (wrap, saturate, each `on_decrease`), and the write conversions.
6. Several samples of one point in one delivery give that many change
   entries, in order.
7. Time: LOCAL_TIME and the receive time without a device time; a device
   time converted from a local zone, in the repeated and the skipped hour
   (TIME_UNCERTAIN); `trust: unknown` gives TIME_UNCERTAIN; a device time
   older than the last one is written as received.
8. A dependent point with a failed companion is BAD; a plan never separates
   them.

Scheduling

9. Orders before due polls; groups by priority, then by due time.
10. Block merging: N adjacent points send the planned number of requests;
    `no_read`, `atomic`, `exclusive` and `side_effect` ranges are honoured.
11. A merged request refused as an illegal address uses its split form once.
12. Pacing: no two requests to a device closer than its interval; write
    spacing kept with writes first; with N dead devices on a channel a live
    device keeps its period (probe budget, 7.4).
13. Keep-alive: an idle device gets a request outside every side-effect
    range.
14. Clear-on-read: a `side_effect` range is read once per poll; never
    retried, never read by keep-alive, probe, identity or `READ_NOW`; a lost
    answer is not followed by a second read.

Failure handling

15. Demotion after `demote_after` failed requests (retries included in one),
    probing at `backoff`, recovery; timeouts within `startup_grace` do
    not count; devices sharing a unit id share the count.
16. A lost connection sets every device of the channel BAD with COMM_FAIL
    at once; a refused connection is `CONNECTING` with COMM_FAIL; the
    back-off resets after the first valid answer, not after connect.
17. STALE after `max_age`, cleared by the next good read, checked at every
    boundary.
18. Identity mismatch with `refuse`: `MISMATCH`, tags BAD without COMM_FAIL,
    writes `CONFIG_MISMATCH`.
19. A gateway that cannot reach a unit demotes that unit only; a busy answer
    and a sentinel-only answer do not demote; cached values with a health
    rule give COMM_FAIL.
20. Device restart from a configured indication: `INITIALISING`, a new
    identity check, selections end.

Write requests

21. Each check of 8.2 has a test that fails a request with its status and
    reason, and the device sees nothing. Source against mode includes the
    O-2 case and every fail-closed case.
22. Selections: every rule of 8.6, including a mode change ending a
    selection and a DIRECT refused on a selected output.
23. Expiry: a request expiring in the worker's queue is EXPIRED and nothing
    is sent; a list interrupted by expiry is `PARTIAL`.
24. **A write that times out is never delivered later** (7.3): the simulator
    stops reading during a write, resumes after the expiry, and never sees
    the frame; the result is TIMEOUT `NO_RESPONSE`; the connection was
    closed and reopened; the write is not sent again.
25. No retry: a write is sent once whatever its outcome; a connection lost
    during a write gives `NO_RESPONSE`, and nothing is sent again after the
    reconnect; a write refused before any byte left is `COMM_FAIL`.
26. A whole-word command: the request log shows no read of the register
    before the write, and one write.
27. Rate limits refuse the request over the limit from every source and
    count at the first byte sent.
28. A request taken by a driver killed before it answers is never executed
    twice (TD conformance test 17, with a driver).

Generation change, reload, traces

29. **System-call trace** of 2.5 on the `scada-io` scan thread: only the
    calls of 13.3 over polls, writes, a HAL error and a generation change,
    with the longest time in each call recorded. Not under sanitizers;
    native runners only.
30. A generation change with a request queued, one taken and not started,
    one in flight: ABORTED by the library, ABORTED `NOT_STARTED`, executed
    once and answered before detach; none acted on after detach; no answer
    in the new generation; an old selection cannot be operated. After every
    attach every tag is written BAD with NOT_INIT before the first request,
    except restored ones.
31. Reload: each row of section 12 applies as stated, within 2 s; a refused
    reload leaves the old configuration running.

Configuration and fuzzing

32. `configure` refuses each error of its binding with its CS code, file and
    line (the driver half of "invalid configuration is rejected with file
    and line", reference 18.2).
33. Fuzz targets, run in the `fuzz` job: each protocol's `frame_state` and
    `decode` (Modbus TCP, Modbus RTU), and each protocol's `configure` over
    generated bindings.

Diagnostics

34. Every tag of section 11 exists with its type; counters publish at most
    once a second.

Tests of room parts are added with the phase that implements them.

**The 300-device test (#108), proposed composition:** 300 simulated Modbus
devices on 30 channels: 20 TCP channels of 10 devices behind one simulated
gateway each (unit ids 1 to 10, pacing 100 ms on two of them), 8 TCP
channels of one device each, and 2 RTU channels of 10 devices each over
pseudo-terminals; three poll groups (1 s, 5 s, 30 s); 10 devices dead for
the whole run on the gateway channels, and 20 that die and recover. The
assertion: every live device's achieved `period.<group>` is within its
group's tolerance, with the database size and the driver's memory reported.

## 16. What phase 2 implements

| Part | Section | `libscada-driver` (#104) | Modbus (#105, #106) | `scada-io` (#107) |
| --- | --- | --- | --- | --- |
| Frame protocol calls | 2.2, 2.3 | Calls them | Implements | n/a |
| HAL calls | 13 | Calls them | n/a | Implements |
| Threads, in-process queues, signals, logging queue | 2.4, 11.3 | Must | Inherits | Inherits |
| Channel watchdog | 2.6 | Must | Inherits | Inherits |
| Real-time tier | 2.5 | Must (for `scada-io`) | No: normal scheduler | Must |
| Session protocols, report-driven, helper threads | 2.7 | Room (helper queue: for unbounded I/O channels) | No | Helper for unbounded channels |
| Device states 0 to 6, identity | 3.5, 3.6 | Must | Inherits | States only |
| Several devices per channel, shared unit ids | 3.4 | Must | Must | n/a |
| Sample processing, golden vectors | 5.1 to 5.6 | Must | Decode | Decode |
| Device time from companions | 5.8 | Must | Must | n/a |
| Replay, cause, events lost | 5.7, 5.9 | Room | No | No |
| Scheduling, planning, pacing, keep-alive, side effects | 6.1 to 6.6 | Must | Plans | Poll groups only |
| Activities, scan groups, time sync | 6.1, 6.7, 6.8 | Room | No | No |
| Timeouts and frame time, serial lines, TCP rules, demotion, STALE, restart | 7 | Must | Must | STALE, HAL failure |
| Checks, kinds, value conversion, encodings, gateway select, expiry, results, no retry, rate limits, read-back | 8.1 to 8.11, 8.14 | Must | Encodes and classifies | Writes outputs |
| Native select, two-phase writes | 8.6, 8.8 | Room | No | No |
| Refresh; fail-safe writes of protocol drivers | 8.12, 8.13 | Room (phase 5) | No | n/a |
| Local fail-safe behaviour, start and crash | 8.13, 13.4 | n/a | n/a | Must |
| Generation change, restart, process start and stop | 9, 10 | Must | Inherits | Inherits |
| Diagnostics, logging | 11 | Must (rows marked phase 5 or room aside) | Reports | Reports |
| Reload | 12 | Must | `configure` | `configure` |
| Simulators | 14 | Test protocol | Must | Simulator backend |

The mandatory part of a phase 2 protocol is seven functions: `configure`,
`device_caps`, `point_caps`, `plan`, `encode`, `frame_state`, `decode`.

## 17. Boundaries with the configuration schemas contract

CS owns every key, default and validation code; this contract owns
behaviour. The sections of CS cited: 4.1 (common part and protocol part,
X-8), 4.6 (channel and device keys), 4.7 (point keys), 4.8 (identify), 5.3
(counters), 7.1 (areas), 7.2 (outputs), 10 (codes), 12.3 (owners and
queues), 12.5 (generated tags), 4.10 (shared keys and defaults), 4.11
(timeout), 7.4 (fail-safe keys), 13 (Apply), annex A (Modbus; A.4 read
ranges, A.5 write encodings), annex B (`io`), annex C (shapes).

| Id | Point | CS | This contract |
| --- | --- | --- | --- |
| B-1 | Point address | Common keys, protocol fragments, the Modbus fragments (annex A) | `configure` as the second validator (the same code in `scada-conf`); the raw sample and the processing after decode (5.1) |
| B-2 | Templates (X-8) | Option (b) now | The protocol sees only its binding |
| B-3 | Acquisition | `poll_groups`, `max_age`, `acquisition` (`poll` only in phase 2) | STALE (7.5), scheduling (6) |
| B-4 | Read rules | `read:` ranges per table and unit id: `no_read`, `atomic`, `exclusive`, `side_effect` | Planning and their meaning (6.3, 6.6) |
| B-5 | Outputs and commands | Output keys, kinds, `storage` (required), `write_limits`, `refresh` (setpoint only), `on_shutdown`, `on_control_loss`, `release` and `safe_state` with their domain, `native_select`, `termination_timeout`, `feedback`; the write encoding in `address`; the mode coding | Checks, kinds, conversion, results, fail-safe behaviour (8) |
| B-6 | Identity | `identify:`, `on_mismatch`, the pattern form | Comparison and `MISMATCH` (3.6) |
| B-7 | Sessions and secrets | `session:` reserved; a secret only as `{ secret: <name> }`, refused in phase 2 | Secrets never logged or in tags; a process may read only the secrets its own files reference |
| B-8 | Tag set | Process, owner and queue names (`io` and `io.scan`), ring capacity per owner from its tag count, the 126-source and 4,096-queue limits, the generated tags of section 11 | Which thread writes which tag (2.4, 11) |
| B-9 | Channels | `type`, `role`, endpoints reserved, `rs485` reserved, literal addresses only, `local_address` reserved | 3.4, 7.2, 7.3 |
| B-10 | Unit ids (X-4) | Overlap on (unit id, table, register); 0 to 255 on TCP and `rtu_over_tcp`, 1 to 247 on RTU | 3.4, 7.4 |
| B-11 | Timing and pacing | `timeout` (as defined in 7.1), `retries`, `busy_retries` (reads), `demote_after`, `backoff`, `reconnect`, `connect_timeout`, `keepalive`, pacing, `max_pending_writes`, `lost_after_timeouts`; the frame-time warning CW-608 | Their behaviour (6, 7) |
| B-12 | Apply | CS 13: versions under `/var/lib/scada/conf/versions/<n>/`, the `active` link renamed atomically, `scada-conf` running each protocol's `configure` and `plan` (refusals CF-650 to CF-699), one expanded file per driver stamped with `config_hash` and `config_id`, the watch on the link, who starts a driver | What the driver loads and reports (10, 12) |

Secrets (B-7): **review by a person.** A secret is referenced by name and
fetched at connect from the secrets store (reference 13.3) through a
service that decides by the rule above. The framework passes it to the
protocol in a buffer it zeroes after the call; never logged, never in a
tag. Sessions with secrets are not in phase 2: **open: needs the product
owner's ruling (R-8)**. Committee recommendation: accept.

## 18. Open rulings

Each is drafted with the option shown and marked at its place; a person
decides. The committee's recommendation is from its review of 0.1. Rulings
on keys, the active tree, the version store and `config_hash` are in CS.

| Id | Section | Question | Drafted | Other options | Committee |
| --- | --- | --- | --- | --- | --- |
| X-1 | 8.6 | Native select on a protocol the reference lists without one (6.4) | Native select per device and point; gateway select for the rest; Modbus native select room | Keep 6.4 literally | Accept |
| X-2 | 3.4 | Channel direction and role | `role: connect \| listen` reserved, `connect` only; client or master only | Listening channels now; a server role | Accept |
| X-3 | 3.4 | Main and backup connections | A channel owns an endpoint list (room) | A device under two channels; none | Accept |
| X-4 | 3.4 | Device addresses on a channel (12.5) | Unit id per point; shared unit ids without overlap; shared failure count | Slices as one device; sub-devices | Accept |
| X-5 | 8.12 | Refresh of setpoints and "no audit, no action" | Covered by the request's record; setpoint only; ends on any failed condition, visibly; no automatic re-establishment | A record per refresh; no refresh; `scada-cmd` re-posts with a system user | Accept, with the end visible |
| X-6 | 5.9 | Replayed events | Deferred to phases 3 and 7; sample fields kept as room | (a) with STALE; (a'); (b) | Defer |
| X-7 | 8.6 | Device windows shorter than the operator's select | Per output `at_select` or `at_operate` | Gateway select never longer than the window | Accept |
| X-8 | 4 | Per-protocol parts of a template | One `protocol:` per template on a protocol-free base; several per template later | Several now; independent templates | Accept |
| X-9 | 8.13 | Safe or release value for remote outputs | The framework can do `hold`, `safe` and `release` | Stop refreshing only | Accept |
| X-10 | 2.2 | The protocol interface in phase 2 | A frame protocol: one request per call, the framework owns the exchange | The reference's `ProtocolDriver` | Accept |
| X-11 | 7.5 | STALE | "Not confirmed within `max_age`" | Literal: not refreshed | Accept |
| X-12 | 6.8 | Who sets device clocks | A framework activity, off by default, gated on the unit's sync; none in phase 2 | Never | Accept |
| X-13 | 6.4 | Unservable pacing | Warning | Error | The reviewers differ |
| X-14 | 5.4 | Enumerated feedback | Double point INT64 0 to 3; `scada-cmd` compares enumerations (phase 5) | Two BOOL tags | Accept |
| X-15 | 4 | Vendor point lists | Generated files accepted by CS; importers outside the contract | An importer in phase 2 | Accept |
| X-16 | 8.3 | Settings and setting groups | A setting-group switch is a `trigger`; settings edits out of scope | Both out | Accept |
| X-17 | Annex A | Modbus functions 1 to 6, 15, 16 | The minimum set, with a per-device function list (CS) | Literal | Accept |
| R-1 | 5.8 | Marking of a driver-stamped event time | LOCAL_TIME | TIME_UNCERTAIN; both; none | Accept |
| R-2 | 5.6 | Change latches | The latch as its own tag | Synthesised events | Accept |
| R-3 | 2.5 | Real-time rules for an I/O scan; real-time tier in phase 2 | `scada-io` only in phase 2; the hardware exchange unannotated between `SCADA_RT` parts; the check and tests as stated | A non-real-time I/O thread feeding the real-time one; a change to `CLAUDE.md` | Accept |
| R-4 | 8.8 | `WriteResult.protocol_error` packing | `(reason << 16) \| raw`, read as within "driver-specific" | Raw code only; the reason in a tag | Accept |
| R-5 | 8.13 | A fail-safe action and its audit record | The action never waits; journalled best-effort after | Journal first, else stop refreshing only (0.1) | Accept |
| R-6 | 5.2 | SUBSTITUTED for values substituted by configuration or forced at the device | Accepted, HMI text "substituted" | A companion tag | Accept |
| R-7 | 5.1 | Bit-identical scaling on both architectures | Floating-point contraction off for scaling and conversion code (D-98) | A tolerance in the tests | Accept |
| R-8 | 17 | Sessions with secrets in phase 2 | Out of phase 2; room | In phase 2 | Accept |
| R-9 | 19 | A device's communication heartbeat register (reference 14.4: "should serve it") | Not in phase 2; a device key reserved (CS) | Serve it in phase 2 | Accept |
| R-10 | 7.3 | Host names for TCP channels | Literal addresses only in phase 2; names reserved | Names resolved off the worker | Accept |
| O-2 | 8.2 | AUTO writes while `scada-cmd` is down | Allowed on the stored mode AUTO when the only defect is OWNER_DOWN | Refused (fail closed, 0.1) | Accept |
| O-3 | 2.6 | A stalled channel restarts every channel of its protocol | Accepted for phase 2; separate processes per channel group room | Per-owner liveness first (#116) | Accept, with the room |

## 19. Differences from the reference document

1. The protocol interface is a frame protocol, not reference 6.1's
   `ProtocolDriver` (X-10).
2. Native select may exist on a protocol the reference lists without one
   (6.4; X-1).
3. A device may span unit ids, and devices may share one (12.5; X-4).
4. STALE means "not confirmed within `max_age`" (6.2; X-11).
5. Drivers write safe or release values to remote outputs (6.5 and 14.4
   give safe states to `scada-io`; X-9), and a fail-safe action does not
   wait for its audit record (5.3, 8.2 step 8; R-5).
6. Local outputs default to `safe` on shutdown and on loss of control
   (reference 6.5), remote outputs to `hold` (reference 14.4).
7. A driver has a command thread besides its workers (6.1 names only
   workers), and a channel watchdog that ends the process: one stalled
   channel restarts every channel of its protocol (O-3).
8. AUTO writes continue on the stored mode while `scada-cmd` is down
   (8.1 states only the mode comparison; O-2).
9. Commands to a demoted or refusing device are refused (8.2, check 8).
10. Reference 4.3 puts "any driver assigned to a fast poll group" in the
    real-time tier; phase 2 has only `scada-io` there, and synchronous scan
    groups (6.2) are later (R-3).
11. A driver may refresh a setpoint (8.12) beside `scada-control`'s refresh
    of reference 7.2 step 3; an output uses one of them.
12. The communication heartbeat register of reference 14.4 is not served in
    phase 2 (R-9).
13. Host names are not accepted in phase 2 (R-10).

## 20. Change log

| Version | Date | Change |
| --- | --- | --- |
| 0.1 | 2026-11-22 | First draft (issue #99, P2-02), from the outline in `docs/contracts/README.md`, reference sections 6, 8.1 and 14.4, TD 1.0, issue #84, and the device survey of 2026-11-22 (not in the repository). Decisions D-81 to D-92 |
| 0.2 | 2026-11-22 | Committee review of 0.1 (C++, SCADA and Linux reviewers). The protocol interface is one request per call, the framework owns the exchange (2.2, D-93). Keys, defaults and codes are CS's; this contract cites them. Real-time tier in phase 2 is `scada-io` only; the `SCADA_RT` check stated as it is; allocation and system-call tests (2.5). Request queues per process and source, as reference 5.1. Watchdog ends the process (2.6). Reload builds aside and swaps (3.2). Raw pattern after byte and word order; non-finite values; exponent and integer arithmetic; counters (5). Side effects on register ranges (6.6). Timeout defined, frame time, serial lines, TCP rules: abortive close on a write timeout, consecutive timeouts, idle close, back-off, literal addresses (7). Command kinds, value conversion, selection on mode change, DIRECT on a selected output, `local_blocks_auto`, demoted devices refused, results retried, rate count at the first byte, refresh setpoint only and visible, one fail-safe table, fail-safe without waiting for audit (8). Process start and stop (10). Logging (11.3). Local I/O (13). Simulators and conformance tests extended; a composition for #108 (14, 15). Replayed events deferred (5.9). Decisions D-82 to D-99. Migration: none; no code depends on 0.1 |

## Annex A. Modbus (phase 2)

The protocol-specific rules of the Modbus driver (#105, #106), as far as
behaviour depends on them. The keys and their schema are CS annex A.

**Addresses.** Explicit table and register, `address_base` per template, no
hidden "4xxxx" arithmetic; 0 to 65,535. Unit ids 0 to 255 on TCP and
`rtu_over_tcp`, 1 to 247 on RTU; unit 0 on TCP is a normal unit. Broadcast
is not used.

**Functions (X-17).** Reference 6.4's "function codes 1 to 6, 15, 16" is the
set the driver supports, not a set every device has. A device's or
template's function list and the substitutions it allows are CS's; the
driver never uses a function the device excludes.

**Frame checks** (`frame_state`, `decode`). TCP: protocol id 0; length 2 to
254 and equal to the bytes received; transaction id equal to the request's
(an answer to an earlier transaction is discarded and counted); unit id
and function echoed, or the exception form (function + 0x80, one code
byte); byte count equal to what the request implies; nothing beyond 260
bytes. RTU: the same without the header, with the CRC, nothing beyond 256
bytes. A frame that fails is a broken answer: `COMM_FAIL` (7.1).

**Types.** As CS annex A: integers and floats of 16, 32 and 64 bits, 8-bit
halves, strings, separate byte and word order, values at odd addresses,
halves from separate registers, bit and field points, a value with a
validity bit.

**Block planning.** At most 125 registers or 2,000 bits per read, 123
registers or 1,968 bits per write, configurable lower; no merging across
tables, even where a device mirrors one table in another.

**Answers to outcomes.**

| Answer | On a read | On a write | Demotion |
| --- | --- | --- | --- |
| 01 illegal function | `ABSENT` for the request's points | REJECTED, `NOT_SUPPORTED` | No |
| 02 illegal address | On a merged read, the split form once (6.3); points refused again `ABSENT` | REJECTED, `INVALID_VALUE` | No |
| 03 illegal value | `ABSENT` (a read that splits an aligned value) | REJECTED, `INVALID_VALUE` | No |
| 04 server failure | `DEVICE_INVALID`, BAD; if every request of a full poll period after `startup_grace` gets it, the device is `REFUSING` | REJECTED, `REJECTED_MODE` | No |
| 06 busy | No sample; the next poll reads again | REJECTED, `BUSY` | No |
| 0A, 0B gateway | `COMM_FAIL` for that unit only | REJECTED, `COMM_FAIL` | Yes, that unit |
| Other codes | `DEVICE_INVALID`, BAD; counted | DEVICE_ERROR, `DEVICE_FAULT` | No |
| Timeout, broken frame | `COMM_FAIL` | TIMEOUT, `NO_RESPONSE` (after send; 7.3) | Yes |

**Transport.** As 7.2 and 7.3. RTU framing over TCP (`rtu_over_tcp`) has no
transaction id: the rules of 7.2 for late answers apply, and a gateway that
forwards with delay may still deliver an answer late; the worker reconnects
after a timeout on such a channel.

**Writes.** Function per point; fixed quantity and values; whole-word
masks without read-modify-write; clock blocks in one function 16 request;
the encodings of CS annex A.5. A device-side select over coils (one relay
in the survey) is room (X-1): its procedure is not in the documents
supplied.

**Device time.** Time companions (`source_time_from` with `epoch` or
`datetime`) are phase 2. Event records and record readers are room: a
register that moves an event pointer or clears on read is marked
`side_effect` or `no_read` from the first release, so that no poll can
corrupt a device's event pointer.

**Not in phase 2:** ASCII framing, UDP, vendor functions, broadcast.
