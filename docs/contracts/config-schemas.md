# Contract: configuration schemas

| | |
| --- | --- |
| Contract version | 0.4 (draft) |
| Tree format | 1 (section 11) |
| Status | Draft for review (issue #98, P2-01). Version 0.4 answers the committee's third pass on 0.3. No code for P2-03 to P2-06 starts before a person has reviewed it. |
| Implements | Reference architecture 1.1, sections 12, 7.3, 7.4, 7.6, 8, 9.1, 10.1, and the configuration lines of 4.3, 6, 14.4 and 17.7 |
| Agrees with | Tag database contract 1.0 (sections 3, 5, 8, 12, 13.2, 14 and 14.1); driver interface contract (`driver-interface.md`, #99, drafted at the same time) |
| Owners of this contract | `scada-conf`, `libscada-conf` |

This contract fixes the configuration tree that people write by hand. It has
four parts:

- what each file may contain: the keys, their types, defaults and limits,
  written as tables (sections 3 to 9, annexes A and B);
- the rules a schema cannot express: template expansion, the merge order,
  name rules, the expression grammar, cross-file validation with error codes
  (sections 4, 5 and 10);
- how the tag-set definition of the tag database and its `config_hash` are
  computed from a valid tree (section 12);
- how a committed tree reaches the services (section 13).

**This contract owns every configuration key.** Every key name, type,
default and validation code of the configuration tree is defined here. The
driver interface contract defines what the driver framework and the
protocols do with them, cites them by name, and gives no default of its own.
Section 4.10 lists the keys both contracts use.

The tables are normative. P2-03 (#100) turns them into JSON Schema files, one
per file type, under `libs/scada-conf/schemas/`. When a table and a schema
file disagree, the table wins and the file is fixed (D-69). Annex C lists
every key that takes several shapes.

The words **must**, **should** and **may** are used as in the reference
document. "Phase 2" means what P2-03 to P2-07 build. A key or value marked
**reserved** is part of the format but refused by the validator of this
version (error CF-210), so a later version can accept it without a format
change (section 11). A place marked **open: needs the product owner's
ruling (X-n)** or **(O-n)** is drafted with the option this document
proposes, and stays open until the owner rules. X-n refers to the list of
conflicts in the device survey of 2026-11-22 (not in the repository); O-n
to section 15. Section 15 lists every such ruling with its options and the
committee's recommendation.

Device facts quoted in this document come from the device survey of
2026-11-22 (not in the repository) and are described generically. Every
address, register number and name in the examples is invented.

## Contents

1. File tree, YAML subset and errors
2. Common definitions
3. `system.yaml`
4. Templates and device instances
5. Tags and expressions
6. Logging
7. Areas, outputs, interlocks and safe states
8. Alarms
9. Control
10. Validation rules and error codes
11. Versioning and migration
12. Tag set and `config_hash`
13. Apply: how a committed tree reaches the services
14. Conformance tests
15. Open rulings
16. Differences from the reference document
17. Change log

Annex A: the Modbus protocol schema. Annex B: the `io` protocol schema.
Annex C: keys with several shapes.

## 1. File tree, YAML subset and errors

The tree is the set of files of one configuration version. Where the
versions are kept, and how one becomes active, is section 13. `scada-conf`
validates a whole tree, never a single file alone.

| Path | Schema | Content | Section |
| --- | --- | --- | --- |
| `system.yaml` | `system` | Unit identity, node role, time, network, storage, upstream, email, limits, data plane, real-time settings. Required | 3 |
| `areas.yaml` | `areas` | Areas and their control authority | 7.1 |
| `templates/<name>.yaml` | `template` | One device template per file. No subdirectories | 4 |
| `drivers/<protocol>/<channel>.yaml` | `channel` plus the protocol's schema | One channel and its devices per file | 4.6 |
| `tags/*.yaml` | `tags` | Internal, manual and virtual tags | 5 |
| `enums.yaml` | `enums` | Value tables shared by templates and tags | 5.5 |
| `outputs/*.yaml` | `outputs` | Outputs and interlocks | 7 |
| `alarms/*.yaml` | `alarms` | Alarms not given with a point or tag, priorities and classes | 8 |
| `logging.yaml` | `logging` | Aggregate tiers and logging policies | 6 |
| `control/tasks.yaml` | `tasks` | Tasks and the order of their programs | 9.1 |
| `control/*.yaml` (other than `tasks.yaml`) | `program` | Function block programs | 9.2 |
| `control/*.st` | ST grammar | Structured-text programs | 9.3 |
| `security.yaml` | reserved | Roles, password and session policy, TLS (phase 4) | |
| `notifications.yaml` | reserved | Email rules and recipients (phase 7) | |
| `screens/*.json`, `symbols/*.json` | reserved | Screens and symbols (phase 8) | |
| `link.yaml`, `drivers/link/*.yaml` | reserved | Node link (phase 11) | |

- `<protocol>` is the name of a protocol schema: `modbus` (Annex A) and `io`
  (Annex B) in phase 2. A protocol name is at most 10 bytes of
  `[a-z0-9]`. A directory under `drivers/` with another name is CF-101.
- `<channel>` is the channel's name, and the file name must equal the
  `channel.name` key (CF-102). `<name>` of a template file must equal its
  `template` key (CF-102). A file name is the name plus `.yaml`, and
  templates have no subdirectories, so a reference finds its file without a
  search.
- A reserved file, or any file not listed above, is CF-101. Names that start
  with `.` are not exempt: an editor's backup file in the tree is an error,
  so that what is validated is exactly what is committed.
- The tree holds regular files and directories only. A symbolic link, a
  FIFO, a socket or a device node is CF-101; files are opened without
  following links and never outside the tree's root. A path is ASCII, its
  components follow the grammar of the table (`[a-z0-9_]` plus the
  extension), and `/` is the separator; anything else is CF-101. An
  imported archive (reference 12.7) whose member has `..` or an absolute
  path is refused before anything is extracted.
- User accounts, password hashes and secret values are never in the tree
  (reference 12.1, 13.3). A secret is referenced by name (section 2.6).
- The files that `scada-conf` writes beside a tree (expanded files, the tag
  set, the name map, metadata) are not part of it and are not validated as
  configuration (section 13.2).

### 1.1 The YAML subset

Decision D-70. A file must be in this subset of YAML 1.2. Anything outside it
is CF-110, with the line and column of the first offending token. How a
scalar gets its type is section 1.2, not a YAML schema.

| Allowed | Not allowed |
| --- | --- |
| UTF-8 without a byte order mark; LF or CRLF line ends | Other encodings; a NUL byte; other control characters outside strings, except tab in comments |
| One document, without `---` or `...` markers | Several documents; directives (`%YAML`, `%TAG`) |
| Block and flow mappings and sequences | Anchors (`&a`), aliases (`*a`), merge keys (`<<`) |
| Keys that are plain or quoted scalars | Complex keys (`? `), keys that are collections, duplicate keys after typing (CF-111) |
| Plain, single-quoted and double-quoted scalars; literal (`\|`) and folded (`>`) block scalars | Tags (`!x`, `!!str`) |
| Comments (`#`) | `null`, `~` or an empty value; a key must have a value (CF-112) |

Reason: anchors, merge keys and tags let one line change values far away and
make "file and line" of an error ambiguous; templates (section 4) are the
mechanism for reuse.

### 1.2 Scalar typing

Every scalar is read as text first, with its quoting style. Its type is then
decided by the schema of the key it is the value of (or, for a map key, by
the key type the schema gives that map). No YAML schema is involved, so the
same text means the same thing whatever parser is used.

| Schema type | Plain scalar accepted | Quoted scalar | Example |
| --- | --- | --- | --- |
| string, name, identifier, text | The text as written | The text | `label: 0123` is the string `0123` |
| integer | `-?(0\|[1-9][0-9]*)` or `0x[0-9a-fA-F]+`, no sign on hex | Refused (CF-203) | `register: 0x10` is 16 |
| decimal | `-?(0\|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?` | Refused | `gain: 0.1`, `limit: 1e3` |
| number (integer or decimal) | Either form; the written form is kept (below) | Refused | `gain: 10` is an integer, `gain: 10.0` a decimal |
| boolean | `true`, `false` exactly | Refused | `True`, `yes`, `on` are CF-203 |
| duration, size, percentage | Section 2.2 | The same text | `5s` |
| expression | Any text | Any text | `"ct_primary / 1000"` |
| number or expression | A number by the rules above, otherwise an expression | An expression | `gain: 2` is a number, `gain: "k * 2"` an expression |

- An integer must fit its key's range (section 2.3); a decimal must be finite
  in binary64, and a decimal that is not zero must not round to zero
  (`1e-400` is CF-203, not 0). `+5`, `1.`, `.5`, `010`, `1_000`, `0o17` and `0b1` match no
  rule and are CF-203 where a number is expected; where a string is
  expected they are strings.
- **The written form is kept.** A decimal literal whose value is a whole
  number (`10.0`) stays a decimal. Where a rule depends on whether a number
  is an integer (the derived value type of section 4.7), the written form
  decides.
- Map keys are typed the same way. A map whose schema has integer keys
  (`bits`, enumerations, `map` of a quality rule, `states`, `values`) reads
  `0`, `3` and `0x10` as integers; `"01"` quoted is refused there. A map
  whose schema has boolean keys (`values` of a BOOL point) takes `true` and
  `false`. Two keys that type to the same value (`16` and `0x10`) are
  CF-111.

### 1.3 Limits

Each limit is checked before the work it bounds, so that a hostile or broken
tree (an imported archive, reference 12.7) cannot exhaust the validator.
Exceeding one is CF-113, naming the limit.

| Item | Limit |
| --- | --- |
| A file | 8 MiB |
| A tree | 20,000 files and 256 MiB |
| Nodes (scalars and collections) of a file | 1,000,000 |
| Nodes of a tree | 4,000,000 |
| Nesting depth of a file | 32 |
| Entries of one map or sequence | 65,536 |
| A scalar | 64 KiB |
| A key | 64 bytes |
| A `.st` file | 1 MiB, nesting depth 32, 100,000 statements |
| An expression (configuration or run time) | 4,096 bytes, depth 32 |
| A range of `repeat` | 65,536 values, checked before it is iterated |
| Points of one template after expansion | 65,536 (CF-414) |
| Tags of the expanded tree | `limits.max_tags`; expansion stops when it is passed (CF-1001) |
| Errors and warnings reported | The first 1,000 of each, then a count |

### 1.4 Errors, their location and their order

Every error and warning names a file, a line and a column (1-based), a code
and a fixed message (section 10). A value that came from a template names the
instance first and then the template file and line it came from (#103):

```
drivers/modbus/line1.yaml:14:7: error CF-402: tag name ps1.pump_station_number_one.fm1.flow_total_since_commissioning is 131 bytes, the limit is 127
  from template templates/flow_meter_x.yaml:22:9 (point flow_total_since_commissioning)
```

Validation runs in stages. A stage runs only when the stages before it found
no error, except stage 3, which checks every file that parsed in stage 2.

| Stage | What | Codes |
| --- | --- | --- |
| 1 | The tree: paths, file kinds, sizes | CF-101 to CF-103; CF-113 for the file and tree size limits |
| 2 | Parsing of every file | CF-110 to CF-114; CF-113 for the node, depth, entry, scalar and key limits |
| 3 | Schema of each file (before expansion; expression strings are checked as text) | CF-2xx (CF-212 on a name written in the file) |
| 4 | Expansion of templates and instances | CF-4xx; CF-113 for the `repeat` limit; CF-1001 when the expanded tags pass `limits.max_tags` (expansion stops); CF-212 on a name made by expansion |
| 5 | Names, references and cross-file rules of the expanded tree | CF-3xx, CF-5xx to CF-9xx |
| 6 | The tag set, which gives the `config_hash` the expanded files carry | CF-10xx, but CF-1001, CF-1012 and CW-1013 |
| 7 | Each protocol's check mode on the expanded files, run by `scada-conf` (section 13.1) | CF-650 to CF-699, CF-1012, CW-1013 |

A code belongs to the stage in which it arises; the table lists where each
can arise, and the corpus (test 5) pins it. Within a stage, errors are
sorted by file path (bytes), line, column and code; the cap of 1,000
reported errors (section 1.3) applies in that sorted order, so the same tree
always reports the same first 1,000. Warnings follow the same order and are reported only when no stage
gave an error. A tree with an error is not committed (reference 12.4). A
tree with warnings only can be committed; the warnings are shown and kept
with the commit.

## 2. Common definitions

### 2.1 Names

| Name | Grammar | Length | Examples |
| --- | --- | --- | --- |
| Tag name | Segments joined by `.`; a segment is `[a-z0-9][a-z0-9_-]*` | 1 to 127 bytes (tag database contract 14), after expansion (section 4.5) | `ps1.pump2.flow`, `sys.drv.line1.state` |
| Point name (in a template) | One or more segments, as a tag name | 1 to 127 bytes; the full tag name is checked | `flow`, `meter.power`, `pv3.voltage` |
| Device name | As a tag name. It is the prefix of the device's tags | 1 to 100 bytes (below) | `ps1.fm1` |
| Identifier | `[a-z][a-z0-9_]*` | 1 to 31 bytes | template, policy, area, channel, task, program, block, alarm, parameter, enumeration and interlock names |
| Poll group name, tier name | `[a-z0-9][a-z0-9_]*`, not all digits | 1 to 8 bytes | `fast`, `1m`, `15m` |
| Protocol name | `[a-z0-9]+` | 1 to 10 bytes | `modbus`, `io` |
| Unit id | `[a-z0-9][a-z0-9-]*` | 1 to 31 bytes | `site-0042` |

- Tag names are unique across the unit (reference 12.2). A user tag name must
  not start with the segment `sys`, which is kept for generated system tags
  (CF-301). A user tag name must not end in a segment that a generated tag
  appends to it (section 12.5) when that would make two tags of one name
  (CF-302).
- Channel names are unique across the unit. Owner and queue names of the tag
  database are at most 31 bytes; they are generated from channel and task
  names by the short-name rule of section 12.3, so the names users write are
  not shortened for them (D-75).
- **Device names are at most 100 bytes**, so that the longest generated
  system tag of a device, `sys.dev.<device>.<name>` with the 18-byte names of
  the driver interface contract, fits 127 bytes. Every generated name is
  still checked (CF-403).
- Free text (`description`, `message`, `guidance`, `label`, `unit`) is UTF-8
  without control characters, at most 1,024 bytes, `unit` at most 32 bytes.
  A `label` holds a device-side identifier as the device documents it (for
  example a reference of up to 129 characters, case sensitive), never
  forced into the tag-name grammar.

### 2.2 Durations, sizes and percentages

- A **duration** is a string: a whole number without sign followed by a unit,
  `ms`, `s`, `m`, `h`, `d`, or `y` (365 days, for retention only). Examples:
  `100ms`, `5s`, `90d`, `10y`. No fractions and no compound forms: write
  `1500ms`, not `1.5s` or `1s500ms`. Each key gives its range; `0s` only where
  the key allows it.
- A key whose name ends in `_ms` or `_s` takes a whole number of that unit, as
  in the reference examples (`alarm_offset_ms: 500`). This form is used where
  the value goes unchanged into the tag set (`heartbeat_check_ms`).
- A **size** is a string: a whole number followed by `B`, `KiB`, `MiB`,
  `GiB` or `TiB`.
- A **percentage** is a string of a decimal number followed by `%`: `0.5%`.
  Where a key takes "a number or a percentage" (`deadband`), a percentage
  is relative to the tag's `range` (section 5.1), which must then be given
  (CF-505).

### 2.3 Numbers and values

- Integers are 64-bit. A key typed `uint64` accepts 0 to 2^64 − 1; every
  other integer key accepts −2^63 to 2^63 − 1, narrowed by its stated range.
  A **raw pattern** (`invalid_values`, `raw` values of section 7.2) accepts
  0 to 2^64 − 1 in decimal or hexadecimal, and a negative decimal, read as
  the two's complement of the raw type's width.
- Decimal numbers are read as the nearest binary64 value. Where exactness
  matters (scaling), a key offers an integer form (`divide_by`, section 4.7).
- A value for a tag (a safe value, a substitute, a limit) must fit the tag's
  type: a BOOL takes `true` or `false`, an INT64 or UINT64 an integer in
  range, a FLOAT64 any finite number.

### 2.4 Configuration expressions

Some keys of templates take an **expression** over constants, template
parameters and `repeat` variables, evaluated during expansion (section 4.5,
step 7): `when`, addresses, `scale`, `repeat` ranges, `identify` values. A
key typed "number or expression" accepts a number or a string, and a string
is an expression (section 1.2). The grammar is that of section 5.6, with the
additions for configuration expressions given there (strings, `in`,
ranges). An expression that refers to an unknown name, has the wrong type,
divides by zero or overflows is CF-410.

```yaml
when: "has_meter"
scale: { gain: "ct_primary / 1000" }                 # REAL: 250 / 1000 is 0.25
address: { table: holding, register: "100 + 2 * (n - 1)", type: int16 }
```

### 2.5 Patterns

A **name pattern** matches tag or point names: `*` matches any characters
within one segment, `?` exactly one character of a segment.
`ps1.inv*.power` matches `ps1.inv1.power` and `ps1.inv12.power`, not
`ps1.inv1.dc.power`. Name patterns are resolved during validation, never at
run time. A name pattern that matches nothing is CF-311.

A **text pattern** (identity checks, section 4.8) matches free text: `*`
matches any characters, `?` one character, `\` makes the next character
literal; the whole text must match. It has no other operators, so matching
takes time linear in the text and the pattern. It is not a regular
expression.

### 2.6 References

| Reference | Form | Resolved against |
| --- | --- | --- |
| Tag | A tag name | Every tag of the expanded tree, generated tags included (section 12.5) |
| Point (inside a template) | A point name | The points of the same template after expansion |
| Template, policy, area, enumeration, interlock, program, task | An identifier | The definitions of that kind |
| Poll group | A poll group name | The poll groups of the device's channel |
| Secret | `{ secret: <identifier> }` | The secrets store (reference 13.3) |

A reference that does not resolve is CF-310. A **secret** reference is the
only form a secret may take: a key that a schema marks "secret" refuses a
plain string (CF-213). Phase 2 has no secrets store, so a secret reference
anywhere is CF-214 ("secrets are not available in this version").

The store's key is held by root (reference 13.3), so an unprivileged service
asks a privileged one for a secret. The rule that makes such a request
decidable from the tree: **a process may read exactly the secrets that the
files it loads reference**, as listed in its expanded file (section 13.3).
The secret's value never enters the tree, an expanded file, a tag or a log.
## 3. `system.yaml`

```yaml
unit:
  id: site-0042
  name: Pump Station 1
time:
  timezone: Etc/UTC
  sync:
    mode: ntp
    servers: [10.0.0.1, 10.0.0.2]
    alarm_offset_ms: 500
    alarm_unsynced_s: 600
storage:
  free_space_alarm: 10%
data_plane:
  heartbeat_check_ms: 250
```

| Key | Type | Required, default | Meaning |
| --- | --- | --- | --- |
| `format` | integer | 1 | Tree format, 1 for this contract (section 11). It may be left out while the tree is of format 1, as the example of reference 12.3 is; a tree of a later format must state it |
| `unit.id` | unit id | required | Identity of the unit: audit, upstream, node link |
| `unit.name` | text, at most 64 bytes | required | Display name |
| `node.role` | `standalone`, `master`, `slave` | `standalone` | Reference 17.1. `master` and `slave` are reserved until phase 11 |
| `time.timezone` | IANA zone name | required | For display and schedules only; every stored time is UTC (reference 12.2) |
| `time.sync.mode` | `ntp`, `ptp`, `none` | `ntp` | Reference 12.3 |
| `time.sync.servers` | list of host names or IPv4 or IPv6 addresses, 1 to 8 | required unless `mode: none` | The unit's time client resolves the names itself; no driver worker is involved, so the literal-address rule of TCP channels (O-7) does not apply |
| `time.sync.alarm_offset_ms` | 1 to 60,000 | 500 | System alarm above this offset |
| `time.sync.alarm_unsynced_s` | 10 to 86,400 | 600 | System alarm when not synchronised for this long |
| `network.hostname` | host name | the unit id | Applied by `scada-sys` (reference 12.6) |
| `network.interfaces[]` | list | none | `name` (interface name), `zone` (`field` or `management`, reference 13.6), `addresses` (list of CIDR), `gateway`, `dns` (list) |
| `storage.free_space_alarm` | percentage | `10%` | Reference 9.4 |
| `conf.keep_versions` | 5 to 1,000 | 50 | Committed versions kept in the version store (section 13.2) |
| `storage.journal_reserve` | size | `2GiB` | Space kept for journals and configuration (reference 14.4) |
| `upstream` | map | absent: no upstream | `url` (HTTPS), `auth` (`certificate` or `token`, each a secret), `order` (`newest_first` or `oldest_first`, default `newest_first`), `rate_limit` (size per second), `window` (`from`, `to` as `HH:MM` in the unit's zone). Used from phase 10 |
| `email` | map | absent: no email | `server`, `port` (default 587), `tls` (`starttls` or `implicit`, default `starttls`), `username`, `password` (secret), `from`. Used from phase 7 |
| `limits.max_tags` | 1 to 1,000,000 | 50,000 | Tags of the tag set, generated tags included (CF-1001). The tag database limit is 1,000,000 |
| `limits.max_devices` | 1 to 10,000 | 1,000 | Devices of the unit (CF-1002) |
| `limits.max_devices_per_channel` | 1 to 256 | 247 | CF-1002 |
| `limits.max_channels` | 1 to 1,000 | 300 | Channels of the unit (CF-1002). Each channel is an owner of the tag database, which allows 1,023 owners in all (tag database contract 14) |
| `data_plane.heartbeat_check_ms` | 1 to 60,000 | 250 | How often `scada-tagd` checks heartbeats and retain files; written into the tag set (section 12). **In a tag set with a `retain_now` tag, `heartbeat_check_ms` must be at most 400 ms, so that a RETAIN_NOW change is on disk within 1 s: two checks plus a write of 200 ms fit in it, (1000 − 200) / 2** (tag database contract 12, D-62). A larger value is refused with "heartbeat_check_ms must be at most 400 in a tag set with a retain_now tag" (CF-1010). Every tree with an output or an area has `retain_now` tags (section 12.5), so in practice the limit always applies |
| `data_plane.retain_interval_ms` | 1,000 to 60,000 | 5,000 | Retain interval of `scada-tagd` (`--retain-interval-ms`, tag database contract 12). Not in the tag set |
| `data_plane.generation_change_limit` | duration, `1s` to `60s` | `5s` | Time limit of the coordinated restart (reference 5.5, 19.2) |
| `data_plane.processes.<process>.heartbeat_timeout_ms` | 1 to 600,000 | 3,000; 500 for a real-time process | Per process (section 12.2) |
| `data_plane.processes.<process>.control_attach_grace` | duration, `0s` to `600s` | `60s` | For a driver process and `io`: after the writer attaches to a generation, control counts as lost only once `scada-control` has attached in that generation, or once this time has passed, whichever comes first (driver interface contract 8.13.2). It keeps the local default `{ action: safe, after: 0s }` from defeating `at_start: hold` and `on_generation_change: hold` |
| `data_plane.event_queue_capacity`, `data_plane.command_queue_capacity` | power of two, 16 to 1,048,576 | 1,024 | Capacity of each event queue and of each command and result queue (section 12.3) |
| `realtime.cpus` | list of CPU numbers | none | CPUs for every real-time thread of every process: control tasks and the `scada-io` scan (reference 4.3). A CPU the unit does not have is not an error: the threads then run without isolation and a system alarm is raised at start (reference 4.3), because the list depends on the hardware as the uids do |
| `realtime.io_priority` | 2 to 98 | 95 | `SCHED_FIFO` priority of the `scada-io` scan thread. It is above every control task (section 9.1), so that a task never waits for a scan that it preempts |

- Change ring capacities are not configured here: each owner's ring is sized
  from its own tag count (section 12.3).
- `heartbeat_check_ms`, the heartbeat timeouts and the capacities are in the
  tag set, so a change to them changes `config_hash` and restarts the data
  plane (section 12.7). The editor says so before the commit (D-26).
- Network, time and security changes need the confirm-or-revert step of
  reference 12.4.

## 4. Templates and device instances

With hundreds of devices, templates are the main configuration tool
(reference 12.2). A template describes one device type once. An instance in a
driver file gives a name, which is the prefix of its tags, the protocol's
device address, parameter values and overrides.

A hand-written template for a simple device is the form of reference 12.3,
with one key added (`bit_order` is not needed here: no point uses a bit):

```yaml
# templates/flow_meter_x.yaml
template: flow_meter_x
protocol: modbus
points:
  - name: flow
    address: { table: holding, register: 100, type: float32, word_order: big }
    unit: m3/h
    scale: { gain: 1.0, offset: 0.0 }
    poll_group: normal
    max_age: 5s
    logging: { policy: process_default, interval: 1s, raw_retention: 180d }
    alarms:
      - { type: high, limit: 450, deadband: 5, on_delay: 10s, priority: medium }
  - name: total
    address: { table: holding, register: 102, type: uint32 }
    unit: m3
    poll_group: slow
    kind: counter
    logging: counters
```

### 4.1 Common part and protocol part

Open: needs the product owner's ruling (X-8). This draft takes option (b) of
the survey, with room for (a).

- A point has a **common part**, defined by this contract (section 4.7): its
  name, value type, kind, unit, enumeration, bits, alarms, logging, retain,
  poll group, ages, access, scaling and validity rules.
- A point has a **protocol part**, exactly one key, `address`. Its content is
  defined by the protocol's address schema (Annex A for Modbus, Annex B for
  `io`) and is opaque to the rest of this contract. `scada-conf` validates it
  against that schema and then runs the protocol's own `configure` on it
  (section 13.1).
- Channel and device objects follow the same rule: the framework keys of
  this contract and the protocol keys of the protocol's schema stand side by
  side in one object. The two key sets are disjoint, checked when the
  schemas are built; a key in neither is CF-201.
- The framework applies the common part to what the protocol decodes:
  exponent, scaling, invalid markers, bits, enumerations, counters, quality
  rules (driver interface contract). The protocol only reads and assembles
  the raw value.
- A template has at most one `protocol`. A template without `protocol` is a
  **base template**: common parts only, no `address`, no `read`, no
  `identify` (CF-421). A device type that speaks several protocols is a base
  template with the signals, and one template per protocol that `extends` it
  and adds the addresses, the scaling and the validity rules, which differ
  between protocols for the same signal. A protocol template removes the
  signals of its base that its protocol does not carry with its own
  `disable` (section 4.4).
- Room for (a): a later version may accept, in one template, a
  `bindings:` map from protocol name to a whole binding per point (address,
  scaling, invalid markers, quality rules). It is reserved (CF-210).

Reason: one protocol per template keeps the simple Modbus template of
reference 12.3 exactly as it is, and a base template already gives one place
for names, units, alarms and logging of a multi-protocol device.

### 4.2 Template keys

| Key | Type | Required, default | Meaning |
| --- | --- | --- | --- |
| `template` | identifier | required | Equals the file name |
| `description` | text | none | |
| `source` | map | none | Metadata, kept and shown, never interpreted: `document`, `revision`, `file`, `sha256`, `generated_by`, `min_protocol_version` (all text) |
| `extends` | template reference | none | Section 4.3 |
| `protocol` | protocol name | none: a base template | Section 4.1 |
| `params` | map of identifier to parameter | none | Section 4.4 |
| `defaults` | point object without `name` and `when` | none | Merged under every point (section 4.5, step 5). May hold a partial `address` (for example `word_order`) |
| `enums` | map of identifier to enumeration | none | Local value tables (section 5.5) |
| `points` | list of point objects | none | Section 4.7 |
| `repeat` | list of repeat blocks | none | Section 4.4 |
| `disable` | list of point names and name patterns | none | Points of the base templates that this template removes (section 4.4) |
| `bit_order` | `lsb0`, `msb0` | none; required as soon as a point uses a bit, a field or `bits` (CF-618) | How bits are numbered within a raw value. There is no default: devices say both, and a wrong guess reads a plausible wrong bit |
| `read` | protocol read rules | protocol default | Annex A.4 for Modbus |
| `functions` | protocol key | protocol default | Annex A.1 for Modbus: the functions the device type supports |
| `device_time` | as for a device (4.6) | `{ zone: utc, trust: unknown }` | How the device type's time values are read; an instance or an address may override it |
| `keepalive`, `idle_timeout` | as for a device (4.6) | none | Device type facts; an instance may override them |
| `identify` | list of identity checks | none | Section 4.8 |
| `on_mismatch` | `warn`, `refuse` | `refuse` | What an identity mismatch does (section 4.8) |
| `restart_indication` | list | none | How a restart of the device is recognised (section 4.9) |
| `health` | list of health rules | none | Section 4.9 |
| `symbol` | identifier | none | Default symbol, phase 8 |
| `session` | reserved | | Steps at connect and restart with secrets. Out of phase 2 (open: needs the product owner's ruling (O-9)) |

### 4.3 `extends`

Decision D-71: a template may extend one other template (single parent), to
a depth of at most 4 parents. There is no other form of inclusion. A cycle
is CF-401.

- A base template may extend only a base template. A protocol template may
  extend a base template or a template of the same protocol (CF-421).
- What a child inherits, and how it changes it, is the merge order of section
  4.5.

Reason: a shared core of a device family and a protocol-free signal base are
both single inheritance. Composition from several parents would need an order
between unrelated parents and gives a point two origins; options that a
device has or not are parameters (`when`), not included templates.

### 4.4 Parameters, `when`, `disable`, `only` and `repeat`

```yaml
params:
  inputs:    { type: int, min: 1, max: 32, default: 16 }
  has_meter: { type: bool, default: false }
  options:   { type: set, values: [battery, meter] }
points:
  - name: meter.power
    when: "'meter' in options"
    address: { table: holding, register: 300, type: int32 }
repeat:
  - for: { n: "1..inputs" }
    points:
      - name: "pv{n}.voltage"
        address: { table: holding, register: "if(n <= 24, 1000 + 2 * (n - 1), 1400 + 2 * (n - 25))", type: int16 }
        scale: { divide_by: 10 }
        unit: V
```

- A **parameter** has a `type` (`int`, `number`, `bool`, `string`, `enum` with
  `values`, `set` with `values`), an optional `default`, and `min` and `max`
  for numbers. An instance gives values in `params`. A parameter without a
  default and without a value is CF-411; a value of the wrong type or out of
  range is CF-412. A `set` value is a list of its `values`; `'meter' in
  options` is true when the string is in the list (section 5.6).
- `when` on a point, or on a repeat block, is a configuration expression of
  type BOOL. A point whose `when` is false does not exist: it creates no tag
  and is not read. Use it for options, cards, models and firmware variants,
  above all where an absent option reads as 0 instead of failing. `when` may
  not be given in `defaults` or `overrides` (CF-419), because presence is
  decided before they are merged.
- **`disable`** in a template, or in an instance, lists point names or name
  patterns to remove. A name or pattern that matches no point is CF-413, so
  that a typing error does not leave a point active.
- **`only`** in an instance, or in a template, lists the point names and
  name patterns to keep; every other point is removed. It is for large
  templates of which a site uses a few points, and for site-chosen maps
  where the instance places the points it uses (C-10 of the survey). In a
  template it keeps only the listed points of what the levels above gave,
  so that thirty instances of one site map share one list. `only` and
  `disable` in one instance, or in one template, are CF-207. An entry of
  `only` that matches nothing is CF-413.
- A **repeat block** has `for`, a map of one or two variables to ranges, an
  optional `when`, and `points`. A range is `"a..b"` (inclusive, integer
  expressions) or a list of integers and ranges, for irregular index sets:
  `[1, 2, "5..8"]`. A range whose end is below its start is empty, so
  `"25..inputs"` with `inputs` of 20 gives no point. Each range has at most
  65,536 values and two variables at most 65,536 pairs, checked from the
  bounds before anything is produced (CF-113). For two variables the points
  are produced for every pair, the first variable outermost. Inside the
  block, `{n}` in a point name is replaced by the value of `n`.
- Irregular address layouts (a run that continues at another base, a reserved
  register inside a run) are written with `if(...)` in the address
  expression, or as several repeat blocks with their own ranges.

### 4.5 Expansion and the merge order

Decision D-72. An instance is expanded into its final list of points in these
steps, in this order. Every merged value keeps the file and line it came
from, so an error names the instance and the template line (section 1.4).

1. **Chain.** The `extends` chain is resolved, from the root template down to
   the instance's template.
2. **Parameters.** The parameter declarations of the chain are collected
   (a child adds parameters, or changes the `default`, `min` or `max` of an
   inherited one, never its `type`, CF-415), and the instance's `params`
   replace the defaults.
3. **Levels.** For each template of the chain, from the root down:
   1. `defaults`, `read` and `enums` (by name; a child's table replaces the
      inherited table of the same name whole) are merged into what the
      levels above gave; so are `bit_order`, `functions`, `device_time`,
      `keepalive`, `idle_timeout`, `on_mismatch`;
   2. its `repeat` blocks are expanded: each produced point records the
      values of the block's variables, which stay bound to it for step 7.
      When a point produced at this level is merged into one an upper
      level produced (step 3.3), the two sets of bindings are joined; a
      variable bound to two different values is CF-422;
   3. its points, those written and those from `repeat`, are merged by name
      into the inherited points: a point of the same name is merged into the
      inherited one, the child's keys winning; a new name is added. Two
      points of one name within one level are CF-416;
   4. its `disable` removes inherited points, or its `only` keeps only the
      points it matches (a template's own points included);
   5. `identify`, `health` and `restart_indication`: a level that gives the
      key replaces the inherited list whole.
4. **Presence.** Every point whose `when` is false is removed (a `when` is
   evaluated here, with the parameters and the point's variables), then
   every point the instance's `disable` matches, or every point its `only`
   does not match.
5. **Defaults.** The template's `defaults`, merged with the instance's
   `defaults` (instance winning), are merged under every point: a key the
   point gives wins.
6. **Overrides.** The instance's `overrides` (a map from point name to a
   partial point) are merged into the points, the override winning. An
   override of a point that does not exist, or that step 4 removed, is
   CF-413. The instance's `identify` entries are appended to the template's
   (a serial number is an instance fact).
7. **Evaluation.** Every remaining configuration expression of a point is
   evaluated, wherever it came from (the template, `defaults`, an override),
   with the parameters and the variables bound to that point. A point that
   no `repeat` produced has no variables.
8. **Prefix.** Each point's tag name becomes `<device name>.<point name>`;
   its bits get theirs (section 5.4). Names are checked (CF-402, CF-403).
9. **Validation** of the expanded tree, with every rule of section 10.

**How two values merge.**

- Two maps are merged key by key, recursively; any other value (a scalar, a
  list) replaces the one before it whole, so that the result does not depend
  on positions in two files. An alarm list of a point is therefore replaced
  whole by an override that gives `alarms`.
- **Exclusive groups.** Some keys exclude each other. Giving one key of such
  a group removes the inherited keys of the same group, so that an
  override can change form:

  | Group | Keys |
  | --- | --- |
  | Scaling | `gain`, `divide_by` (in `scale`) |
  | Bit selection (an address) | `bit`, `field`, the shapes of a double point (`on`, `off`, `fault`) |
  | Register (Modbus address) | `register`, `parts` |
  | Write encoding (Modbus `write`) | `value`, `values`, `on` and `off`, `mask` |
  | Invalid action | `bad`, `keep_last`, `substitute` (as values of `on_invalid` or `action`) |
  | Feedback | the forms of `feedback`, and `no_feedback` |

- **Replacing a map whole.** A map given in a later level, a default or an
  override may hold `replace: true`; it then replaces the inherited map
  instead of merging into it, and the key `replace` itself is dropped. For
  example `overrides: { ia: { address: { replace: true, table: input,
  register: 3, type: int16 } } }`. `replace` is a reserved key name in every
  map of a point.
- There is no way to delete a single key other than these two; points are
  removed by `when`, `disable` and `only`.

An instance that names no template gives `points` directly (reference 12.1,
"or lists its points"); steps 1 to 3 are then empty, and `params`, `disable`
and `only` are refused (CF-201). An instance may give `points` and a
template together: its points are added after step 6 and a name that already
exists is CF-416.

### 4.6 Channel files and device instances

```yaml
# drivers/modbus/line1.yaml
channel:
  name: line1
  type: rtu
  device: /dev/serial/by-path/platform-serial1
  baud: 19200
  parity: even
  poll_groups:
    fast:   { period: 200ms, priority: 1 }
    normal: { period: 1s,    priority: 2 }
    slow:   { period: 10s,   priority: 3 }
devices:
  - { name: ps1.fm1, template: flow_meter_x, unit_id: 11, timeout: 300ms, retries: 2 }
  - { name: ps1.fm2, template: flow_meter_x, unit_id: 12, timeout: 300ms, retries: 2 }
```

Framework keys of a channel (the protocol's keys are in its schema):

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | identifier | required | Equals the file name; unique across the unit |
| `enabled` | bool | `true` | A disabled channel is not connected. Its tags and system tags stay in the tag set (BAD, NOT_INIT, diagnostics with state 0), so that enabling or disabling it is a live change and references to its tags stay valid. The same holds for a disabled device |
| `role` | `connect`, `listen` | `connect` | Who opens the connection. `listen` is reserved (open: needs the product owner's ruling (X-2)). Both are the client or master role of reference 6.4 |
| `poll_groups` | map of poll group name to `{ period, priority }` | `fast` 200 ms, `normal` 1 s, `slow` 10 s, priorities 1, 2, 3 | Reference 6.2. `period` 10 ms to 1 d; `priority` 1 (highest) to 16. A poll group is not real-time by its name: no protocol driver is real-time in phase 2 (section 12.2) |
| `min_request_interval` | duration, `0s` to `60s` | `0s` | Pacing of the channel: least time between two requests on it |
| `min_write_interval` | duration, `0s` to `60s` | `0s` | Least time between two writes on the channel (a gateway that limits writes for all its devices); a device's own interval applies as well |
| `reconnect` | `{ min, max }` durations, `100ms` to `10m` | `{ min: 1s, max: 30s }` | Reconnect back-off after a connection failure, doubling from `min` to `max`. It starts again from `min` after the first valid answer, not after a connect |
| `max_pending_writes` | 1 to 256 | 16 | Write requests the channel holds in its own order queue; one more is answered `BUSY` (driver interface contract) |
| `lost_after_timeouts` | 1 to 10 | 3 | `tcp` channels only; on `rtu_over_tcp` a read time-out never closes the connection. The rule is the driver interface contract's (7.3), quoted word for word: "The worker keeps one run per connection. A read request that times out after its retries (a planned read of a poll group or a keep-alive) adds one to the run and records its unit id; identity reads, `READ_NOW` and probes neither add to the run nor reset it. Any valid answer from any unit, an exception answer included, empties the run. The eligible unit ids are those of the channel that have at least one device in state `INITIALISING` or `RUNNING` with at least one poll group. After each addition, and before the timed-out device's own demotion count is applied, the connection is lost when the run holds at least `lost_after_timeouts` requests and its unit ids include every eligible unit id. A reconnect empties the run; a second close before any valid answer reconnects with the `reconnect` back-off, not at once." A write time-out, a read time-out on a `side_effect` range, or a frame the kernel does not take whole, closes the connection at once whatever this key says (same section). Open: needs the product owner's ruling (R-15 and R-17 of the driver interface contract) |
| `change_ring_capacity` | power of two, 1,024 to 1,048,576 | computed (section 12.3) | Override of the computed ring size of the channel's owner, for channels whose changes come in bursts. At least twice the owner's tags (CF-1003) |
| `process_group` | reserved | | Several driver processes for one protocol (open: needs the product owner's ruling (O-3)) |

Framework keys of a device instance:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | device name | required | Prefix of every tag of the device; unique across the unit |
| `template` | template reference | none | Section 4.5 |
| `params`, `defaults`, `overrides`, `disable`, `only`, `points` | | none | Sections 4.4, 4.5 |
| `enabled` | bool | `true` | As for a channel |
| `timeout` | duration, `10ms` to `60s` | `1s` | Section 4.11 |
| `write_timeout` | duration, `10ms` to `60s` | `timeout` | Section 4.11 |
| `write_timeout_per_value` | duration, `0s` to `10s` | `0s` | Added to `write_timeout` for each value written, for devices whose write time grows with the number of values |
| `retries` | 0 to 10 | 2 | Retries of a failed read. A write is never retried |
| `busy_retries` | 0 to 10 | 3 | Times a read answered "busy" is tried again at later turns before it counts as failed. Reads only: a write answered busy is refused `BUSY` and never sent again by the driver (driver interface contract) |
| `demote_after` | 1 to 100 | 3 | Consecutive failed transactions before the device is demoted (reference 6.2). A read whose retries all failed counts once; "busy", a sentinel answer and failures within `startup_grace` do not count |
| `backoff` | duration, `1s` to `1h` | `10s` | First retry period of a demoted device; it doubles at each failed probe up to `backoff_max` |
| `backoff_max` | duration, `1s` to `1h` | `300s` | Largest retry period of a demoted device |
| `startup_grace` | duration, `0s` to `10m` | `0s` | After a connect or a detected restart, time-outs in this time do not count toward demotion |
| `min_request_interval` | duration, `0s` to `60s` | `0s` | Pacing of the device: least time between two requests to it, on top of the channel's |
| `min_write_interval` | duration, `0s` to `60s` | `0s` | Least time between two writes to the device |
| `max_values_per_request` | 1 to 65,535 | protocol limit | Most values one read or write may carry. A value is one point's raw value. A protocol's own limits (Modbus `read.max_registers`) apply as well; the smaller wins |
| `keepalive` | duration, `1s` to `1h`, or `off` | the template's, else `off` | Send a protocol request to the device when no request has gone to it for this long, for devices that close idle sessions. A protocol request, never a TCP keep-alive probe, because devices count requests, not probes, for their idle timers |
| `idle_timeout` | duration | the template's, else none | The device's own idle time-out, as its documentation states it; informative, used by CW-607 |
| `acquisition` | `poll`, `report` | `poll` | `report` is reserved for the report-driven protocols of phase 9; their report settings are protocol keys |
| `device_time` | `{ zone: utc \| local, tz, trust: unknown \| synced }` | the template's | How the device's time values are read (below) |
| `time_sync` | `{ mode: none }` | `none` | Who sets the device's clock. Other modes (`on_connect`, `periodic` with `period`) are reserved (open: needs the product owner's ruling (X-12)); the device must never be set from a unit whose own time is not synchronised |
| `on_mismatch` | `warn`, `refuse` | the template's | Section 4.8 |
| `identify` | list | appended to the template's | Section 4.8 |

- Several devices may share one connection; each has its own time-out,
  retries and demotion (driver interface contract).
- How several devices on one channel may share or split a device address is
  the protocol's rule (Annex A.3 for Modbus).
- **Device time.** `device_time` may also be given in an address, for a
  device that keeps some time values in UTC and others in local time.
  `zone: local` needs `tz`, an IANA zone; the driver converts to UTC. A local
  time in the hour that is repeated at the end of summer time is read with
  the earlier offset (the first occurrence) and marked TIME_UNCERTAIN; one in
  the hour that is skipped at its start is moved forward by the offset change
  and marked TIME_UNCERTAIN. `trust: unknown` (the default) means the unit
  cannot tell whether the device's clock is set: its time values are
  written with TIME_UNCERTAIN. `trust: synced` declares that the site keeps
  the device's clock synchronised.

### 4.7 Point keys

The keys of the common part. Keys that apply only to some kinds are marked.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | point name | required | |
| `value_type` | `bool`, `int64`, `uint64`, `float64`, `string` | derived (below) | Type of the tag |
| `kind` | `plain`, `counter`, `double_point` | `plain` | Section 5.3 |
| `description`, `label`, `unit` | text | none | `label`: section 2.1 |
| `range` | `[min, max]` numbers | none | Engineering range: percentage deadbands, display |
| `raw_range` | `[min, max]` numbers | none | A raw value outside it is written UNCERTAIN with OUT_OF_RANGE, for devices that saturate without saying so |
| `access` | `ro`, `rw`, `wo` | `ro` | `rw` and `wo` may be the target of an output (section 7.2). A `wo` point is never read; its tag shows the last value written (driver interface contract) |
| `storage` | `volatile`, `nvm` | required for `rw` and `wo` (CF-619) | `nvm`: a write goes to the device's non-volatile memory, whose write count is limited. There is no default, because devices rarely say which of their registers go to flash, and a wrong `volatile` lets a program wear it out |
| `write_limits` | `{ min_interval, max_per_day }` | none; for `nvm`, `{ min_interval: 1h, max_per_day: 10 }` | Writes beyond them are refused by the driver framework, whatever their source; a write counts when its first byte is sent |
| `dangerous` | bool | `false` | Writing it has effects beyond the point (a reset that acts for every master, a factory reset, a test function). Section 7.2 |
| `invert` | bool | `false` | For a BOOL point: the device's 1 is `false`. Applied on read and on write |
| `poll_group` | poll group reference | `normal` | |
| `max_age` | duration, `10ms` to `1d` | 3 times the poll group period | Beyond it the tag gets STALE (reference 6.2). For reported points the meaning is that of the driver interface contract |
| `update_period` | duration | none | How often the device itself refreshes the value; for validation only (CF-611) |
| `scale` | `{ gain, offset }` or `{ divide_by, offset }` | none | Below |
| `exponent_from` | `{ point, direction: multiply \| divide }` | none | Below; the exponent point is a companion (CF-612) |
| `invalid_values` | list of entries | the template's `defaults` | Raw patterns that mean "no value" (below) |
| `on_invalid` | `bad`, `keep_last`, `{ substitute: <value> }` | `bad` | What an invalid raw value gives (below) |
| `quality_from` | list of quality rules | none | Quality from status bits (below); each point named is a companion |
| `source_time_from` | point reference | none | The point whose value (a time, section A.2) is the source time of this one; a companion |
| `bits` | map of bit number to a bit definition | none | Section 5.4 |
| `tag` | bool | `true` | `false`: the point creates no tag of its own, only its bits or its role as a companion |
| `enum` | enumeration reference or inline map | none | Section 5.5 |
| `counter` | counter rules | none (`kind: counter` only) | Section 5.3 |
| `states` | map | (`kind: double_point` only) | Section 5.3 |
| `when` | configuration expression | `true` | Section 4.4 |
| `alarms` | list of alarms | none | Section 8 |
| `logging` | logging settings | not logged | Section 6 |
| `retain`, `retain_max_age` | | `false`, none | Section 5.2 |
| `address` | the protocol's address | required in a protocol template once expanded (CF-417) | Annexes A and B |

Whether reading a point changes the device (clears a flag, moves a pointer)
is a property of the registers it is read from, not of the point. It is
declared on register ranges in the protocol's read rules (Annex A.4), so that
every point decoded from such a register is served by the same single read.

**Derived value type** (when `value_type` is not given): a bit, a coil or a
`bool` raw type gives `bool`; an integer raw type without `scale` and
`exponent_from`, or with a `gain` and `offset` both written as integers
(section 1.2), gives `int64` (`uint64` for an unsigned raw type of 64 bits);
any other scaling and any floating raw type gives `float64`; a string raw
type gives `string`; `kind: double_point` and `enum` give `int64`. A FLOAT64
holds integers exactly up to 2^53; a 64-bit counter scaled by a fraction is
written as a raw integer point plus a virtual tag. A `value_type` that cannot
hold the raw type is CF-504.

**Scaling.** The engineering value is computed from the raw value in this
order, in binary64 unless every step stays an integer:

1. Exponent: with `exponent_from`, e is the raw integer value of the
   exponent point, from −20 to 20. `multiply` gives raw × 10^e, `divide`
   gives raw ÷ 10^e. A negative power is applied as a division by the exact
   integer 10^|e| (exact in binary64 up to 10^22), never as a multiplication
   by an inexact 10^−|e|. An exponent outside the range, or invalid, makes
   the value BAD.
2. Scale: `{ gain, offset }` gives x × gain + offset; `{ divide_by, offset }`
   gives x ÷ divide_by + offset. `gain` and `offset` are numbers or
   configuration expressions; `divide_by` is a positive integer or an
   expression giving one. Multiplication and addition are two operations,
   never fused, so that x86-64 and ARM64 give the same bits (D-98, the driver
   interface contract's decision; ruling R-7 there).
3. With an integer value type, the computation is in INT64 or UINT64,
   checked: a result that does not fit is written BAD with OVERFLOW. A
   `gain` for which the raw type's whole range times the gain cannot fit
   the value type is CF-516.

The inverse, for written values, is in section 7.2.

**Non-finite values.** A floating raw value that is NaN or infinite and
matches no invalid marker is written BAD with OUT_OF_RANGE, and counted.

**Invalid values.** An entry is `{ raw: <pattern>, action, flags, reason }`.

- `raw` is compared with the value's bit pattern after the protocol has
  assembled it (after byte order, word order and `parts`), before any
  numeric interpretation, exponent or scaling. It is a raw pattern (section
  2.3): a float32 marker is written as its 32-bit pattern (`0x7F800000`, or
  `0x80000000` for −0.0), a marker of an `int32` as `0x80000000` or
  `-2147483648`. The string `nan` matches any NaN of a floating raw type.
- `action` is `bad`, `keep_last` or `{ substitute: <value> }`, default the
  point's `on_invalid`. `bad` writes the tag BAD, with no COMM_FAIL, the
  `flags` given (any of OUT_OF_RANGE, OVERFLOW, BLOCKED, TEST), and does not
  count toward demotion. `keep_last` keeps the last valid value with
  UNCERTAIN and STALE, not SUBSTITUTED: it is a value that was not
  refreshed. Once `max_age` has passed since that value was read, the tag is
  BAD with STALE. `keep_last` needs `max_age` on the point (CF-640).
  `substitute` writes the value with UNCERTAIN and SUBSTITUTED. SUBSTITUTED
  here means "substituted by configuration"; the HMI must not show it as
  "entered by an operator" (tag database contract 6 defines the flag for a
  person's entry; this widens it, section 16, ruling R-6 of the driver
  interface contract). A consumer tells an operator's entry from the other
  meanings by its journal record, not by the flag. There is no `uncertain`
  action (C-29 of the survey): a marker means the device has no value;
  `substitute` gives a stand-in with UNCERTAIN where one is known.
- Two forms are accepted. In a point, `invalid_values` is a list of entries;
  a bare pattern stands for `{ raw: <pattern> }`. In a template's or an
  instance's `defaults`, it is a map from raw type to a list
  (`{ uint16: [0xFFFF], float32: [nan] }`), applied to every point of that
  raw type. A point's own list replaces the default whole, and
  `invalid_values: []` turns it off. There is no system-wide default: a
  pattern that is a marker on one device is a valid value on another.

**Quality rules.** An entry of `quality_from` is
`{ point, bit | field, map }`: the value of `point` (another point of the
template, or `self` for the point's own raw word), or of the given bit or
bit field (`field: [lo, hi]`) of it, is looked up in `map`, whose values are
`good`, `uncertain`, `bad`, or a list of one of these and flags; `default`
covers values not listed. The worst result of all rules and of the value's
own checks is the tag's quality.

**Companions.** A point named by `exponent_from`, `quality_from` or
`source_time_from` is a companion. It must be in the same poll group as the
point that uses it, and the driver reads it in the same scan, before or
directly after that point (CF-612). It may be far from the point in the
device's address space.

```yaml
- name: temp1
  address: { table: input, register: 40, type: int16 }
  scale: { divide_by: 10 }
  invalid_values: [ { raw: 0x7FFF, flags: [OUT_OF_RANGE], reason: "sensor open" } ]
  quality_from: [ { point: status1, field: [8, 11], map: { 0: good, 5: [bad, TEST], default: bad } } ]
```

### 4.8 Identity checks

`identify` is a list of checks made by the driver after connecting and after a
detected restart, before the device's values are published (driver interface
contract). Each check is `{ address | point, equals | in | range | pattern }`:
the value read at `address` (protocol address, read once, not a tag) or the
value of a `point` is compared with a value, a list, `[min, max]` or a text
pattern (section 2.5). The values may be configuration expressions, so that
a count parameter can be checked against the device's own count register.
An address used by a check must not lie in a side-effect range (Annex A.4).
With `on_mismatch: refuse` a failed check puts the device in "configuration
mismatch": its tags read BAD and it is not written. With `warn` the device
runs and the mismatch is a diagnostic and a system alarm. The default is
`refuse`, because a template applied to the wrong variant reads plausible
numbers from the wrong registers.

### 4.9 Health rules and restart indications

`health` is a list of `{ point, good_when, applies_to, comm_fail }`: when the
value of `point` is not `good_when` (a value, a list, or `{ field, equals }`),
the tags in `applies_to` (`all`, or a list of point names and patterns) are
written BAD; with `comm_fail: true` they also get COMM_FAIL. Use
`comm_fail: true` for a device that is lost behind a gateway, `false` for a
device that reports a fault of its own. The health point must be in the same
poll group as the points it applies to, and is read first in the scan
(CF-612). A health point that is itself BAD, or older than its `max_age`,
counts as "not good".

`restart_indication` is a list of `{ point, rule }`, where `rule` is
`decreases` (a run-time or uptime counter going back), `changes` (an identity
or boot counter), or `{ equals: <value> }` (a power-up flag). When one holds,
the driver treats the device as restarted (driver interface contract).

### 4.10 Shared keys and defaults

Decision D-102. The keys below are read by the driver framework or a protocol, whose
behaviour the driver interface contract defines. Their names, types,
defaults and ranges are those of this contract; the driver interface
contract cites them and gives no default of its own.

| Key | Where | Default | Behaviour defined in |
| --- | --- | --- | --- |
| `timeout`, `write_timeout`, `write_timeout_per_value` | device | 1 s, `timeout`, 0 s | Section 4.11 here; driver interface, failure handling |
| `retries`, `busy_retries` | device | 2, 3 | Driver interface, failure handling |
| `demote_after`, `backoff`, `backoff_max` | device | 3, 10 s, 300 s | Driver interface, demotion |
| `startup_grace` | device | 0 s | Driver interface, demotion |
| `reconnect`, `lost_after_timeouts` | channel | 1 s to 30 s; 3 | Driver interface, connection loss |
| `connect_timeout` | channel (Modbus `tcp`, `rtu_over_tcp`) | 3 s | Annex A.1 |
| `min_request_interval`, `min_write_interval` | channel and device | 0 s | Driver interface, pacing |
| `max_values_per_request` | device | protocol limit | Driver interface, block planning |
| `max_pending_writes` | channel | 16 | Driver interface, threads |
| `keepalive`, `idle_timeout` | template, device | off, none | Driver interface, keep-alive |
| `poll_groups`, `poll_group`, `max_age`, `update_period` | channel, point | section 4.6, 4.7 | Driver interface, scheduling and staleness |
| `acquisition` | device | `poll` | Driver interface (reported points, phase 9) |
| `device_time`, `time_sync` | template, device, address | UTC, `unknown`; `none` | Driver interface, time of a sample |
| `identify`, `on_mismatch` | template, device | none, `refuse` | Driver interface, identity |
| `restart_indication` | template | none | Driver interface, device restart |
| `health` | template | none | Driver interface, companions and health |
| `bit_order` | template | none, required with bits | Annex A.2 |
| `functions` | template, device | the protocol's set | Annex A.1 |
| `read` (ranges, limits) | template, device | Annex A.4 | Driver interface, block planning and side effects |
| `scale`, `exponent_from`, `invalid_values`, `on_invalid`, `raw_range`, `quality_from`, `source_time_from`, `bits`, `enum`, `counter`, `states`, `invert` | point | section 4.7 | Driver interface, samples (the framework applies them after decode) |
| `access`, `storage`, `write_limits`, `dangerous` | point | `ro`; required; none; `false` | Section 7.2 here; driver interface, write requests |
| Output keys (`kind`, `sbo`, `select_timeout`, `native_select`, `limits`, `refresh`, `on_shutdown`, `on_control_loss`, `on_generation_change`, `safe_state`, `failsafe_retry`, `failsafe_max_refusals`, `feedback`, `termination_timeout`) | output | section 7.2 | Driver interface, write requests; reference 8 (`scada-cmd`, phase 5) |
| `change_ring_capacity` | channel | computed | Section 12.3 |
| `enabled` | channel, device | `true` | Driver interface, reload |

### 4.11 What a time-out measures

**`timeout`** is the time from the end of the request on the wire to the
first byte of the answer. How the driver turns it into deadlines is the
driver interface contract's (timeouts and the frame time), stated once
there: a request with no first byte by its end on the wire plus `timeout`
has timed out; after the first byte, the expected answer has its frame time
at the channel's character format plus a margin of the larger of 3.5
characters and 20 ms. On a TCP channel the line time is zero; on
`rtu_over_tcp` the serial line behind the gateway is not seen by the
driver, so the `timeout` given must cover the gateway's time on its line.
**`write_timeout`** is the same for a write, plus `write_timeout_per_value`
for each value written.

Warning CW-608 is given when the requests of a poll group, with their line
times and the pacing, cannot be served within the group's period
(section 10.6).
## 5. Tags and expressions

Every tag of the unit is one of these, and each has exactly one owner
(reference 5.4; section 12.4 gives the owners):

| Kind | Defined in | Written by |
| --- | --- | --- |
| Point tag | A point of a device (section 4) | The driver channel |
| Internal tag | `tags/*.yaml`, `type: internal` | The control task that writes it |
| Manual tag | `tags/*.yaml`, `type: manual` | `scada-cmd`, on an operator's entry |
| Virtual tag | `tags/*.yaml`, `type: virtual` | The control task named in it (reference 7.6) |
| Generated tag | Section 12.5 (outputs, areas, alarms, interlocks, parameters, system tags) | The service it belongs to |

### 5.1 Tag files

```yaml
# tags/site.yaml
tags:
  - name: site.pv.total_power
    type: virtual
    expr: sum("ps1.inv*.power")
    unit: kW
    task: normal
    quality: ignore_bad
    logging: process_default
  - name: site.net_power
    type: virtual
    expr: site.pv.total_power - site.load.power
    unit: kW
    logging: { policy: process_default, interval: 1s }
  - name: ps1.tank.level_sp
    type: manual
    value_type: float64
    area: ps1
    limits: { min: 0, max: 5 }
    initial: 2.5
    retain: true
```

Keys of every tag in `tags/*.yaml`:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | tag name | required | |
| `type` | `internal`, `manual`, `virtual` | required | As in reference 12.3 |
| `value_type` | as for points | required for `internal` and `manual`; derived from `expr` for `virtual` | |
| `kind` | `plain`, `counter` | `plain` | Section 5.3 |
| `description`, `unit`, `range`, `enum` | | none | As for points |
| `area` | area reference | none; required for `manual` | Permissions and grouping |
| `alarms`, `logging` | | none, not logged | Sections 8, 6 |
| `retain` | bool | `true` for `manual`, else `false` | Section 5.2 |
| `retain_now` | bool | `false` | `internal` and `manual` only; needs `retain` (CF-506) |
| `retain_max_age` | duration | none | Section 5.2 |

Keys of one kind only:

| Key | Kind | Meaning |
| --- | --- | --- |
| `task` | `internal`, `virtual` | The owning task. Default for `virtual`: `normal`. For `internal`: the task of the program that writes it; given, it must agree (CF-905) |
| `expr` | `virtual` | Expression (section 5.6), required |
| `quality` | `virtual` | `worst` (default) or `ignore_bad` (reference 7.6) |
| `limits` | `manual` | `{ min, max }`: an entry outside is refused by `scada-cmd` |
| `initial` | `manual`, `internal` | Value written when no retained value is restored; without it the tag stays BAD, NOT_INIT until written |

### 5.2 Retained values and their age

- `retain: true` makes the tag RETAIN in the tag set (tag database contract
  12). A STRING tag cannot be retained (CF-507). `retain_now` adds
  RETAIN_NOW and needs `retain` (CF-506).
- `retain_max_age` (#89) is an optional bound on the age of a restored value,
  a duration from `1s` to `3650d`, unset by default. It needs `retain`
  (CF-506). A restored value older than the bound, or whose age is unknown
  (tag database contract 12: another boot without a synchronised clock),
  is restored as BAD with RESTORED, the value kept, so that a reader sees
  it and does not act on it. Within the bound the restore is as today,
  UNCERTAIN with RESTORED.
- The bound is refused (CF-508) on the tags whose restore must never be lost
  to a clock: output mode tags, area authority tags, alarm state and
  shelving tags (all generated, section 12.5), and any tag of
  `kind: counter`.
- The bound is not part of the tag set and not in `config_hash` (D-74). It is
  given to the owner's library at attach, which applies it while it restores,
  before the region is set `ready`, so that no reader sees the old value as
  UNCERTAIN first. A change of the bound alone needs no generation change; it
  applies at the next restore.
- **Depends on tag database 1.1 (#89).** The attach option that carries the
  bounds and the restore rule are an addition to tag database contract 1.0,
  a minor version with a migration note (D-66 f), made by #89 and reviewed by
  a person (restart behaviour). Until 1.1, `retain_max_age` is validated but
  has no effect, and `scada-conf` warns (CW-510).

### 5.3 Counters and double points

- `kind: counter` marks a value that only counts up. `counter` gives
  `limit` (the largest raw value: the device wraps to 0 or stops there),
  `at_limit` (`wrap` or `saturate`, default `wrap`), and `on_decrease`
  (`accept`, `hold`, `reset`, default `accept`).
  - The tag holds the device's value; the driver keeps no running total
    across a wrap (a totaliser in logic or the historian does that).
  - A **wrap** is a decrease from at least `limit / 2` to below `limit / 2`
    with `at_limit: wrap`: the new value is written with no flag.
  - A **saturation** is a value equal to `limit` with `at_limit: saturate`:
    written with OVERFLOW until the value changes.
  - Any other decrease follows `on_decrease`: `accept` writes the smaller
    value; `hold` keeps the last value with UNCERTAIN until the counter
    passes it again (for a device that steps back after a power cut);
    `reset` writes the value and records a counter reset in the journal.
  - `logging: counters` in reference 12.3 is a policy name like any other.
- `kind: double_point` is a position with an INT64 tag that every driver
  writes with the same codes, so that `scada-cmd`, alarms and screens need
  one coding: `0` intermediate, `1` open or off, `2` closed or on, `3`
  faulty. The value 3 is written, with quality BAD, so that a screen shows
  "faulty" and not "no data"; `0` stays GOOD, so a travelling switch is not
  a fault. The protocol's address gives two bits, three bits (on, off and a
  fault bit), two coils, a 2-bit field or an integer (Annex A.2); `states`
  maps the device's raw codes onto the four codes, because devices code the
  same position differently. A map is required for the field and integer
  shapes (CF-613).

### 5.4 Bits

`bits` on an integer point creates one BOOL tag per named bit, from the one
read of the word. Only the bits named exist, so an alarm word of 16 bits with
three used bits gives three tags, not sixteen.

```yaml
- name: alarm_word1
  address: { table: holding, register: 50, type: uint16 }
  tag: false
  bits:
    0: { name: overtemp, alarms: [ { type: state, alarm_on: true, priority: high } ] }
    3: { name: fan_fault }
    7: { name: door_closed, invert: true }
```

A bit definition is a name (point-name grammar; the tag is `<device>.<name>`,
so a bit's name is unique among the device's points, CF-416) or a map with `name`, `invert`, `description`,
`alarms`, `logging`, `retain`. Bit numbers are 0 to 63 within the raw value,
numbered as the protocol's `bit_order` says (Annex A.2).

### 5.5 Enumerations

An enumeration maps integer codes to names: `{ 0: stopped, 1: running, 3:
fault }`. It may be sparse and may have negative codes. It is defined in
`enums.yaml` (`enums: { <name>: <map> }`), in a template's `enums`, or inline
in `enum`. A point with an enumeration has an INT64 tag. A raw value that is
a floating number is first rounded to the nearest integer, and must be within
0.001 of it, or the value is BAD with OUT_OF_RANGE. A code not in the table
is written UNCERTAIN with OUT_OF_RANGE. Names are for display and for state
alarms.

### 5.6 Expression grammar

Decision D-78. Virtual tags, alarm expressions, `suppress_when`, interlock
conditions, the structured text of section 9.3 and the configuration
expressions of section 2.4 use one expression grammar, based on the
expression part of the structured-text subset (reference 7.4, 7.6).

**Lexical rules.**

```
integer      = decimal_int | hex_int ;
decimal_int  = "0" | nonzero { digit } ;
hex_int      = ( "16#" | "0x" | "0X" ) hexdigit { hexdigit } ;   (IEC 61131-3 and C forms)
real         = decimal_int "." digit { digit } [ exponent ] | decimal_int exponent ;
exponent     = ( "e" | "E" ) [ "+" | "-" ] digit { digit } ;
time         = "T#" decimal_int ( "ms" | "s" | "m" | "h" | "d" ) ;
string       = "'" { any character but "'" | "''" } "'" ;   (configuration expressions only)
name_segment = ( letter | "_" ) { letter | digit | "_" } ;
name         = name_segment { "." name_segment } ;      (no space inside; a "." is part of
                                                        a name only when a segment follows)
```

- A `real` needs a digit on both sides of the point: `1.`, `.5` and `1..5`
  as a real do not exist, so `1..inputs` lexes as `1`, `..`, `inputs`.
- **Reserved words.** `AND`, `OR`, `XOR`, `NOT`, `MOD`, `DIV`, `IN` and the
  literals `TRUE`, `FALSE`, `GOOD`, `UNCERTAIN`, `BAD` are case-insensitive
  and reserved only as a whole `name` of one segment: `in`, `Mod` and
  `true` are the words, while `in.flow`, `mod.x` and `ps1.true` are names. A
  tag whose whole name is one such word is written `tag("in")`; a parameter
  or variable with such a name is refused (CF-212). Open: needs the product
  owner's ruling (O-14).
- **Function names are not reserved.** A `name` of one segment followed by
  `(` is a call; any other `name` is a reference. So `flow.rate`,
  `level.min`, `power.avg`, `energy.sum`, and a tag named `sum`, are
  ordinary references. A call of a name that is not in the function table
  is CF-529.
- An integer literal is at most 2^63 − 1 (`9223372036854775807`,
  `0x7FFFFFFFFFFFFFFF`); a larger one is CF-528. `-9223372036854775808` is
  the negation of a literal that is too large, so it is CF-528 too; write
  `-9223372036854775807 - 1`. A real literal that is not zero but rounds to
  zero, or that is too large to be finite (`1e-400`, `1e400`), is CF-528.
- A tag name with a segment that does not fit `name_segment` (a `-`, or a
  segment that starts with a digit) is written `tag("ps1.pump-2.flow")`.
  The argument of `tag` has the lexical form of a `pattern_string` and must
  hold no wildcard.
- Spaces and line ends separate tokens and are otherwise ignored.
- **Tokens that abut.** A number, a `time` or a string must not be followed
  directly by a letter, a digit, `_` or `#`: `3x`, `10ms` (a duration is a
  `time`, `T#10ms`) and `0x1G` are CF-520. A `name` ends at the first
  character that cannot continue it, so `a-b` is `a`, `-`, `b` (a
  subtraction) and a name holding `-` must be written with `tag("…")`.
- **Case.** Reserved words, function names and the `T#` prefix are
  case-insensitive (`IF(`, `Sum(`, `t#5s`); the units of a `time` (`ms`,
  `s`, `m`, `h`, `d`) are lower case only (`T#5S` is CF-520); the `e` of an
  exponent may be `e` or `E`; tag, parameter and variable names are
  case-sensitive, as in section 2.1.

**Syntax.**

```
expression   = or_expr ;
or_expr      = xor_expr { "OR" xor_expr } ;
xor_expr     = and_expr { "XOR" and_expr } ;
and_expr     = cmp_expr { "AND" cmp_expr } ;
cmp_expr     = add_expr [ cmp_op add_expr | "IN" ( range | add_expr ) ] ;
cmp_op       = "=" | "<>" | "<" | "<=" | ">" | ">=" ;
add_expr     = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr     = unary { ( "*" | "/" | "DIV" | "MOD" ) unary } ;
unary        = [ "-" | "NOT" ] pow_expr ;
pow_expr     = primary [ "**" unary ] ;
primary      = literal | call | reference | "(" expression ")" ;
call         = name_segment "(" [ argument { "," argument } ] ")" ;
argument     = expression | pattern_string ;
reference    = name ;
range        = add_expr ".." add_expr ;                   (configuration expressions only)
for_value    = range | expression ;                       (the value of `for` in `repeat`, 4.4)
literal      = integer | real | time | string | "TRUE" | "FALSE" | "GOOD" | "UNCERTAIN" | "BAD" ;
pattern_string = '"' { any character but '"' } '"' ;      (the pattern argument of aggregates, and of `tag`)
```

- A `for` value that is not a `range` must give an INT, which stands for
  the one value it gives (`for: { n: "k" }`); any other type is CF-523.
- `.quality` and `.age` follow a `name` only; for a tag written with
  `tag("…")` the forms are `quality(tag("…"))` and `age(tag("…"))`.
- `range` appears only after `IN` and as the value of `for`, never as an
  operand: `n IN 1..5` and `n IN 1 .. 2 * k` hold a range; `1 + 2..5`,
  `x = 1..3` and a bare `1..5` are CF-520. After `IN`, a range is told from
  a value by the `..` that follows the first `add_expr` (one token of
  look-ahead).
- **Checked mechanically.** The syntax above was transcribed one to one
  into an LALR(1) parser generator, which builds its tables with no
  conflict, so the grammar is unambiguous and needs one token of
  look-ahead. The cases of conformance test 11 for syntax were run through
  it with the expected outcome, including the precedence below.

- A comparison does not chain (`a < b < c` is CF-520).
- Two departures from IEC 61131-3 structured text are deliberate: `**` is
  right-associative (`2 ** 3 ** 2` is 512), and only one unary operator is
  allowed in a row (`NOT NOT a` and `- -x` are CF-520; write `NOT (NOT a)`).
  `**` binds tighter than a unary operator: `-x ** 2` is `-(x ** 2)`, and
  `2 ** -3` is allowed.
- A `reference` is, in a run-time expression, a tag name or a tag name
  followed by `.quality` or `.age`; in a configuration expression, a
  parameter or a `repeat` variable. `x.quality` and `x.age` read the
  quality (`GOOD`, `UNCERTAIN`, `BAD`) and the age (TIME) of tag `x`
  (reference 7.4). If `x.quality` is itself a tag name, the reference is
  ambiguous and is CF-521; `quality(x)` and `age(x)` are the unambiguous
  forms.

**Types.** `BOOL`, `INT` (64-bit signed), `REAL` (binary64), `TIME`
(milliseconds, 64-bit), as reference 7.4. Configuration expressions also have
`STRING` (parameters of type `string` and `enum`, and string literals) and
`SET` (parameters of type `set`). A BOOL tag reads as BOOL, INT64 as INT,
FLOAT64 as REAL; a UINT64 tag reads as INT, and its value above 2^63 − 1
gives a BAD result with OVERFLOW. STRING tags cannot be used (CF-522).

| Operator | Operands | Result |
| --- | --- | --- |
| `+`, `-` (binary) | INT, INT | INT |
| | INT or REAL, REAL or INT | REAL |
| | TIME, TIME | TIME |
| `*` | INT, INT | INT |
| | numbers, at least one REAL | REAL |
| | TIME and INT or REAL (either order) | TIME, rounded half away from zero to whole milliseconds |
| `/` | any two numbers | REAL (true division: `250 / 1000` is 0.25) |
| | TIME, TIME | REAL |
| | TIME, INT or REAL | TIME, rounded as for `*` |
| `DIV`, `MOD` | INT, INT | INT; `DIV` truncates toward zero, `MOD` has the sign of the dividend |
| `**` | numbers | REAL |
| `-` (unary) | INT, REAL, TIME | the same type |
| `=`, `<>` | two values of one type; INT and REAL compare as REAL | BOOL |
| `<`, `<=`, `>`, `>=` | INT, REAL, TIME (numbers with numbers) | BOOL |
| `AND`, `OR`, `XOR`, `NOT` | BOOL only | BOOL |
| `IN` | STRING and SET; INT and a range | BOOL (configuration expressions) |

- BOOL never mixes with a number, and the logical operators take BOOL only:
  `NOT level < 5` on an INT tag is CF-523, not `(NOT level) < 5`. Write
  `level <> 0` or `if(b, 1, 0)`; bitwise operations on INT are functions.
- An INT overflow, a division by zero (also `DIV`, `MOD`), a `sqrt` or `ln`
  of a value out of its domain, or a REAL result that is not finite gives a
  BAD result with OVERFLOW at run time, and CF-410 in a configuration
  expression.

Functions:

| Function | Arguments | Result |
| --- | --- | --- |
| `if(c, a, b)` | BOOL, then two values of one type, or INT and REAL | The type of `a` and `b` (REAL if one is REAL). Only the chosen branch is evaluated; its quality counts with that of `c` |
| `limit(lo, x, hi)` | numbers | REAL if any is REAL, else INT |
| `min(a, b, ...)`, `max(a, b, ...)` | two or more numbers | REAL if any is REAL, else INT. Ordinary maths: every argument counts, also under `quality: ignore_bad` |
| `abs(x)` | INT, REAL, TIME | the same type |
| `sqrt`, `exp`, `ln`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan` | number | REAL |
| `atan2(y, x)` | numbers | REAL |
| `round(x)`, `trunc(x)`, `floor(x)`, `ceil(x)` | REAL | INT (`round`: half away from zero); a value outside INT is BAD with OVERFLOW |
| `to_real(x)`, `to_int(x)` | INT, REAL | REAL; INT as `trunc` |
| `band(a, b)`, `bor(a, b)`, `bxor(a, b)`, `bnot(a)`, `shl(a, n)`, `shr(a, n)` | INT (n from 0 to 63) | INT; `shr` is logical |
| `bit(a, n)` | INT, INT | BOOL |
| `sum`, `avg`, `min_of`, `max_of`, `count_good` | one pattern string, or two or more tag references | `avg`: always REAL. `sum`, `min_of`, `max_of`: INT when every input is INT, REAL when any is REAL (a pattern that matches INT and REAL tags gives REAL). BOOL inputs are refused for these four (CF-523); `count_good` takes inputs of any type but STRING and gives INT. Aggregates: `quality: ignore_bad` leaves BAD inputs out |
| `moving_avg(x, T#5m)`, `rate(x, T#1m)`, `totalise(x, T#1h)` | a tag reference and a TIME literal | REAL. `rate` is change per second over the window; `totalise` integrates `x`, a rate per given time, into a quantity. Each call keeps its own state in the task; the window is 1 to 10,000 task cycles (CF-524) |
| `quality(x)`, `age(x)`, `is_good(x)` | a tag reference | INT (`GOOD`, `UNCERTAIN`, `BAD` are the INT constants 0, 1, 2 of the tag database; compare with `=`, not `<`), TIME, BOOL |
| `tag("name")` | a `pattern_string` with no wildcard | the tag |

The transcendental functions and `**` are not correctly rounded by every C
library, so they may differ in the last bit between architectures
(section 14, test 11 allows it for them alone).

Quality and time of a result (reference 7.6): with `quality: worst` the
result has the worst quality of the inputs it used; with `quality: ignore_bad`
aggregates leave out BAD inputs, and the result is UNCERTAIN if any was left
out, BAD if all were. The source time is the newest source time among the
inputs used. A result sets no quality flag except OVERFLOW.

Patterns in aggregates are resolved at validation (section 2.5). A virtual tag
may use virtual tags; the validator orders them within each task and refuses
a cycle (CF-525), also through a pattern that matches the tag itself. A
virtual tag that uses a virtual tag of another task reads that tag's last
value, as any other input. Expressions are compiled and type-checked at
validation and run under the instruction budget of their task (section 9.1).
The derived `value_type` of a virtual tag follows from the table above:
BOOL gives `bool`, INT `int64`, REAL `float64`; TIME gives `int64`
milliseconds. It enters the tag set, so these rules are part of the format.
## 6. Logging

```yaml
# logging.yaml
aggregate_tiers:
  - { name: 1m,  interval: 1m }
  - { name: 15m, interval: 15m }
  - { name: 1h,  interval: 1h }
policies:
  process_default:
    mode: on_change
    deadband: 0.5%
    max_interval: 5m
    raw_retention: 90d
    tiers: { 1m: 1y, 1h: 10y }
  counters:
    mode: periodic
    interval: 1m
    raw_retention: 1y
    tiers: { 1h: 10y }
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `aggregate_tiers` | list of `{ name, interval }`, 1 to 4 | `1m` and `1h` | Reference 9.1. Intervals from `1s` to `1d`, increasing, each a whole multiple of the one before (CF-801) |
| `default_policy` | policy reference | none | Policy of tags that give no `logging`. Without it, such tags are not logged |
| `policies` | map of identifier to settings | none | Named settings |

Settings (reference 9.1):

| Key | Type | Meaning |
| --- | --- | --- |
| `mode` | `on_change`, `periodic`, `both` | Required |
| `deadband` | number or percentage | `on_change`: least change that gives a sample. Default 0 (every change) |
| `interval` | duration, `10ms` upward | `periodic`, `both`: sample period, required for them. `on_change`: the least time between two samples of the tag, a rate limit (as in the reference 12.3 example, `{ policy: process_default, interval: 1s }` on an `on_change` policy); a change within it is sampled when it ends, so the last value is never lost |
| `max_interval` | duration | `on_change`, `both`: a sample is forced after this time |
| `raw_retention` | duration | Required |
| `tiers` | map of tier name to duration | Tiers kept and their retention; a tier not listed is not kept; an unknown tier name is CF-310 |

The `logging` key of a tag or point takes one of: a policy name; `none`; a map
`{ policy, <settings> }` whose settings override the policy's one by one; or a
map of settings without `policy`, complete by itself (`mode` and
`raw_retention` required, CF-802). A STRING tag is logged `on_change` only
(CF-803).

- Quality changes are always logged (reference 9.1).
- **Retention classes** are derived after every tag's settings are resolved:
  one class per distinct pair (raw retention, tiers map), where the tiers map
  is compared as a set of (tier, retention) pairs. The class name is the
  canonical text `raw=<d>;<tier>=<d>;...`, tiers in the order of
  `aggregate_tiers`, durations in the largest unit that divides them. Sampling
  settings do not affect classes. More than 32 classes is warning CW-804
  (reference 9.2).
- An `interval` finer than the tag's refresh (its poll group period, its task
  period) is warning CW-805 (reference 9.1).
- Logging settings are not in the tag set: a change is applied live
  (reference 9.1).

## 7. Areas, outputs, interlocks and safe states

### 7.1 Areas

```yaml
# areas.yaml
areas:
  ps1:
    description: Pump station 1
    authority:
      default: web
      local_panel: { input: ps1.panel.local, active: true }
      local_blocks_auto: true
    auto_resume: false
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `description` | text | none | |
| `authority.default` | `cli`, `web`, `api` | `web` | Holder of authority when nothing is retained (reference 8.3) |
| `authority.local_panel` | `{ input, active }` | none | The physical local/remote switch (reference 8.3: "bound to a digital input tag"): `input` is a BOOL tag read by a driver, `active` the value that means local (`true` or `false`) |
| `authority.local_blocks_auto` | bool | `false` | Local also blocks automatic commands; needs `local_panel` (CF-718). AUTO requests do not pass `scada-cmd`, so the driver framework enforces it on the input tag itself, not on the area's authority tag (which `scada-cmd` owns): an AUTO request is accepted only while the input reads GOOD and not `active`. An input that is not GOOD blocks AUTO (fail closed; open: needs the product owner's ruling, R-16 of the driver interface contract). Consequence: a failed panel input also blocks automatic stops, so the area's outputs stay as they are until an operator acts in MANUAL or the input is GOOD again |
| `auto_resume` | bool | `false` | After a restart, outputs of the area resume in AUTO; with `false` they return in MANUAL and wait for an operator (reference 14.4) |

Every output, manual tag and alarm names an area. An area is used by at least
one of them, or warning CW-701. Each area gets a generated authority tag
(section 12.5).

### 7.2 Outputs

```yaml
# outputs/ps1.yaml
interlocks:
  - name: ps1.pump2.permissive
    condition: ps1.tank.level > 0.5 AND NOT ps1.pump2.trip
    message: "tank level low or pump tripped"
outputs:
  - name: ps1.pump2.speed_cmd
    area: ps1
    kind: setpoint
    target: ps1.vfd2.speed_set
    feedback: { tag: ps1.vfd2.speed, within: 2.0, settle: 5s }
    limits: { min: 0, max: 100, rate: 10 }
    interlocks: [ps1.pump2.permissive]
    safe_state: { value: 0 }
    on_shutdown: hold
    on_control_loss: { action: safe, after: 2s }
  - name: ps1.cb1.cmd
    area: ps1
    kind: double
    target: ps1.relay1.cb_ctrl
    feedback: ps1.relay1.cb_pos
    sbo: true
    reason_required: true
    interlocks: { on: [ps1.cb1.close_permissive], off: [] }
    safe_state: hold
    on_shutdown: hold                  # explicit: a breaker keeps its position
    on_control_loss: { action: hold }
```

An output is the configured controllable point of reference 8.1. Keys:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | tag name | required | Base of the output's generated tags (section 12.5) |
| `area` | area reference | required | Reference 12.5 |
| `kind` | `single`, `double`, `pulse`, `step`, `setpoint`, `trigger`; `latch` is reserved (CF-210): it behaves as `single` | required | What a command means (table below) |
| `on_only` | bool | `true` for `trigger` and `pulse`, else `false` | Only "on" is commanded; "off" does nothing on the device (resets, group selection) and is refused |
| `target` | tag reference | required | A point with `access: rw` or `wo` (CF-702), not STRING. The tag that write requests name; its tag gets WRITABLE_BY_COMMAND (section 12.5) |
| `feedback` | tag reference, `{ on, off }` of two BOOL tags, or `{ tag, within, settle }` | required unless `no_feedback` | The state the device reports (reference 8.1): one tag, separate on and off tags, a double point, or a read-back with a tolerance and a settle time |
| `no_feedback` | `true` | | Explicit "no feedback" (reference 12.5). The write result is then the only confirmation: `<output>.status` reads ACCEPTED_UNVERIFIED, never OK, and the target's tag shows the last value written, unconfirmed |
| `feedback_timeout` | duration, `100ms` to `10m` | `10s` | Reference 8.2, step 10 |
| `sbo` | bool | `false` | Select before operate by `scada-cmd` (reference 8.2, step 6) |
| `select_timeout` | duration, `1s` to `60s` | `30s` | Reference 8.2, step 6; at most the write request lifetime of 60 s (tag database contract 14) |
| `native_select` | `at_select`, `at_operate` | `at_select` | For a target with a select on the device. Open: needs the product owner's ruling (X-1, X-7) |
| `reason_required`, `reauth` | bool | `false` | Reference 8.2, step 7; 13.2 |
| `limits` | `{ min, max, ranges, rate, release }` | none | Value validation (reference 8.2, step 4), in engineering units: `min`, `max`, or `ranges`, a list of `[lo, hi]` intervals (a union); `rate` (most change per second); `release` (below) |
| `interlocks` | list of interlock references, or `{ on: [...], off: [...] }` | required for kinds other than `setpoint` | Reference 8.2, step 5. The map form gives the interlocks per commanded direction (a close only when an earth switch is open, an open always). Open: needs the product owner's ruling (O-21) |
| `refresh` | duration, `1s` to `1d` | none | `kind: setpoint` only. The driver writes the accepted value again at this period, for devices whose setpoint lapses or that fall back when writes stop. Open: needs the product owner's ruling (X-5) |
| `auto_deadband` | number | 0 | For `setpoint` outputs driven by a program: `scada-control` posts a new AUTO request only when the value moved by more than this (reference 7.2, step 3 posts on change) |
| `safe_state` | per kind: `off`, `on` or `hold` for `single` and `double` (for `double`, `off` is code 1 open, `on` code 2 closed); `{ value }`, `{ raw }` or `hold` for `setpoint`; `hold` only for `step`, `pulse` and `trigger` | required (reference 12.5). A fail-safe key with `safe` and a `safe_state` of `hold` is CF-722 | The state the output is driven to when a rule says "safe". `value` is in engineering units, `raw` is the raw pattern written as it stands (section 2.3). A `step`, `pulse` or `trigger` has no safe action: its `safe_state`, `on_shutdown`, `on_control_loss` and `on_generation_change` must be `hold` (CF-719) |
| `failsafe_retry` | duration, `1s` to `60s` | `5s` | The first interval at which a fail-safe write that was not confirmed is repeated while its condition holds; it doubles after each time-out, up to 60 s (driver interface contract 8.13.4, section 7.4) |
| `failsafe_max_refusals` | 1 to 10 | 3 | After this many refusals of the fail-safe write (as the driver interface contract 8.13.4 counts them), the repeats stop, `<output>.failsafe` stays 2 and an alarm is raised; they also stop when a write limit is spent; a new condition starts them again (8.13.3, 8.13.4) |
| `on_shutdown` | `hold`, `safe`, `release` | `safe` for a target of `scada-io` (reference 6.5), `hold` otherwise (reference 14.4) | Section 7.4 |
| `on_control_loss` | `{ action: hold \| safe \| release, after }` | `{ action: safe, after: 0s }` for a target of `scada-io`, `{ action: hold }` otherwise | Section 7.4: `scada-control` down while the output is in AUTO; `after` from `0s` to `10m` |
| `on_generation_change` | `hold`, `safe` | `hold` | During a coordinated restart of the data plane (reference 5.5, 19.2: "each output's behaviour during the restart" is a setting). An output goes safe anyway when the restart exceeds its limit |
| `task` | task reference | the task of the program that drives it, else `normal` | Owner of the AUTO request tag |
| `termination_timeout` | duration | reserved | For two-phase results of phase 9 (driver interface contract) |

**What each kind carries.** A write request carries one value (tag database
contract 9.4). The driver interface contract says what is sent for each.

| Kind | Value of the request | Written on the device |
| --- | --- | --- |
| `single` | BOOL | The target's value, through its write encoding |
| `double` | INT64: 1 off or open, 2 on or close; the target tag is INT64 with these codes, also on two coils | The `off` or `on` encoding of the target's address (Annex A.5) |
| `pulse` | BOOL `true` only | One write; the device makes the pulse. A pulse timed by the unit (an on write, then an off write after a time) is not in this version: a `duration` on a `pulse` output is CF-720, because the off write may fail and leave the output on. Such a pulse belongs in the device, or in a control program with a timer block that posts the off request again each cycle until it succeeds (open: needs the product owner's ruling (O-16)) |
| `step` | INT64: +1 raise, −1 lower | The `on` (raise) or `off` (lower) encoding |
| `setpoint` | The target's type, in engineering units | The raw value of the inverse scaling (below) |
| `trigger` | BOOL `true` only | One write of the encoding's fixed value |

**From engineering value to raw value.** For a `setpoint`, the raw value is
computed from the requested value v. For an INT64 or UINT64 target with no
`scale`, or whose `gain` or `divide_by` and `offset` are integers (written
as integers, section 1.2; an absent `gain` counts as 1 and an absent
`offset` as 0), it is computed in exact integer arithmetic: the result is
the exact integer (v − offset) ÷ gain or (v − offset) × divide_by, every
step checked, and it must fit the raw type, so a UINT64 target with a
negative offset is not refused for a difference above 2^63 − 1 (with no
`scale`, raw = v exactly): v −
offset must not overflow, and with `gain` it must divide by the gain
exactly; otherwise the request is refused (`INVALID_VALUE`). So values
above 2^53 are written exactly. Every other case is computed in binary64:

- with `scale: { gain, offset }`: raw = (v − offset) / gain;
- with `scale: { divide_by, offset }`: raw = (v − offset) × divide_by;
- with no `scale`: raw = v;
- then rounded half away from zero to an integer for an integer raw type
  (12.5 kW on a `divide_by: 10` register is raw 125, not 124). With a
  decimal `gain` the quotient is that of the binary64 nearest the gain, so
  a value half-way between two raw steps may round either way (12.45 on a
  gain of 0.1 need not give 125); `divide_by` is the exact form and should
  be used where a half-way value matters;
- a request value that is NaN or infinite is refused (`INVALID_VALUE`)
  before any limit is compared; a raw value that is not finite or does not
  fit the raw type is refused the same way.
- Not writable (CF-708): a point with `exponent_from`, and Modbus raw types
  `mod10k`, `epoch`, `datetime`; a `bit` or `field` point unless its
  address has a `write.mask` (Annex A.5); an `enum` point writes its code.
- `limits.release` and `safe_state` say their domain: `{ value: <v> }` is
  scaled as above, `{ raw: <pattern> }` is written as it stands. A release
  value is usually a raw pattern outside the value range (`{ raw:
  0x7FFFFFFF }`), which an engineering value could not express.

Rules (each has its code in section 10):

- A target belongs to exactly one output (CF-703). The target's device is on
  a channel; the output's write requests go to its driver's command queues
  (section 12.3), and the driver routes them to the channel.
- `sbo` with `native_select`: whether a target has a select on the device is
  declared by its protocol at `configure` (driver interface contract), which
  `scada-conf` runs (section 13.1). With `at_select`, `select_timeout` must
  not exceed the device's select window (CF-704); with `at_operate`,
  `scada-cmd` holds the operator's selection and the driver runs the device
  select and operate together at OPERATE, which needs a device window of at
  least twice the device's `timeout` (CF-716).
- A target point that is `dangerous` makes the output dangerous: no program
  may drive it (CF-705), it cannot have `refresh` or a `safe_state`,
  `on_shutdown`, `on_control_loss` or `on_generation_change` other than `hold`, which are its defaults whatever the target (CF-705), and
  `reason_required` and `reauth` are forced true.
- `refresh` only with `kind: setpoint`, a `volatile` target, no `sbo` and no
  `dangerous` (CF-714). A target with `storage: nvm` cannot be driven by a
  program (CF-706).
- A `trigger`, `pulse` or `on_only` output has no off value (CF-707); a
  request of `false` to it is refused (`INVALID_VALUE`), never sent as
  nothing.
- A switching output with a safe action (`single`, `double`) on a protocol
  driver that gives neither `on_control_loss` nor `on_shutdown`, so that
  both are `hold` by default, is CW-721. An explicit `hold` is a decision
  and is not warned; neither are `step`, `pulse` and `trigger` (they must
  hold, CF-719) nor a `dangerous` output (CF-705 forces `hold`). The
  warning: it keeps its state when `scada-control` stops or the
  unit shuts down. The default follows reference 14.4; the alternative, a
  `safe` default for remote switching outputs, is the product owner's
  ruling (O-12).
- Writes are one target at a time: there are no write groups (a curve or a
  table written in one request) in this version (open: needs the product
  owner's ruling (O-15)).
- `release` in `on_shutdown` or `on_control_loss` needs `limits.release`
  (CF-714), and is only for a `setpoint` (CF-723).
- An output whose target is on a channel whose protocol has no write for the
  target's address shape is CF-708.

**Status.** `<output>.status` (section 12.5) has the values of reference 8.1
and one more: 6 ACCEPTED_UNVERIFIED, for a write the device accepted whose
effect nothing confirms (`no_feedback`). A write answered TIMEOUT with no
response sets FAILED with "outcome unknown"; the feedback is still watched
until `feedback_timeout`.

**Refresh state.** An output with `refresh` gets a tag `<output>.refresh`
(section 12.5): 0 no refresh, 1 refreshing, 2 ended without a new request
(the requester went down, a tag-set change, a refused request). Value 2 is a
system alarm (phase 7), because the device then falls back after its own
time (open: needs the product owner's ruling (X-5), which also decides who
re-establishes a standing MANUAL setpoint after a restart).

Feedback with four states (open: needs the product owner's ruling (X-14)): a
`double` output takes a `double_point` tag as feedback; `scada-cmd` compares
the commanded position with codes 1 and 2, treats 0 as still travelling until
`feedback_timeout`, and 3 as a failure.

### 7.3 Interlocks

An interlock (reference 8.2 step 5, 8.4) is declared once, in the
`interlocks` list of any `outputs/*.yaml` file:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | tag name | required | The interlock is also a BOOL tag of this name, true when the command is permitted |
| `condition` | expression of type BOOL | required | Section 5.6 |
| `message` | text | required | The blocking reason given to the operator (reference 8.2) |
| `task` | task reference | `normal` | The task that evaluates the tag |

- The tag is evaluated by `scada-control` like a virtual tag. A condition
  that is not GOOD blocks: the tag is written false with the input's
  quality.
- `scada-cmd` uses the tag as a permissive at steps 5 and 9 of reference 8.2.
  An output block of a program uses it as its interlock input (reference 8.4).
- Device-side preconditions (a local/remote selector on the device, a
  jumper) are interlocks on the tags that report them.

### 7.4 Safe states and fail-safe behaviour

Decision D-102. One set of keys, with one meaning in both contracts. The behaviour is the
driver interface contract's (protocol drivers) and #107's (`scada-io`);
`scada-cmd` and `scada-control` are phases 5 and 6.

| Situation | Key | Values | Delay | Default, `scada-io` output | Default, remote output |
| --- | --- | --- | --- | --- | --- |
| Orderly stop of the writer of the target (the driver or `scada-io`), an update | `on_shutdown` | `hold`, `safe`, `release` | none | `safe` (reference 6.5) | `hold` (reference 14.4) |
| `scada-control` reads as down while the output is in AUTO | `on_control_loss` | `hold`, `safe`, `release` | `after` | `safe` at once (reference 6.5) | `hold` |
| The requester of a refreshed setpoint reads as down (`scada-control` in AUTO, `scada-cmd` in MANUAL) | none | refreshing stops; in AUTO `on_control_loss` then applies; in MANUAL the device's own fallback acts | | | |
| `scada-cmd` reads as down while the output is in MANUAL | none | hold: an operator-held output does not drop at a restart of `scada-cmd` | | hold | hold |
| Coordinated restart of the data plane (a generation change) | `on_generation_change` | `hold`, `safe`; safe anyway when the limit is exceeded, measured by the writer itself (reference 5.5, 19.2; section 13.5, G6) | `data_plane.generation_change_limit` | `hold` | `hold` |
| The unit or its link to the device fails | none on the unit | the device's own communication-loss behaviour, set on the device (reference 14.4) | | | |
| After a restart | `areas.<area>.auto_resume` | outputs are not driven until a full control cycle with valid inputs and the restore of retained modes and setpoints; AUTO resumes per area | | A GPIO output must be given a level when it is requested: its `at_start` key (Annex B) | |

The behaviour of each row is the driver interface contract's (protocol drivers,
`scada-io`); its table cites these keys and defaults.

- `safe` drives the target to its `safe_state`; `release` writes
  `limits.release`; `hold` leaves the device as it is and, for a refreshed
  setpoint, stops refreshing.
- **When a fail-safe action is taken, repeated and ended** is defined once,
  as a state machine per output and per key, in the driver interface
  contract: 8.13.1 the cases and defaults, 8.13.2 control lost and the other
  conditions, 8.13.3 the states of an output and `<output>.failsafe`,
  8.13.4 fail-safe writes and their repeats, 8.13.5 the audit record. This
  contract defines only the keys and cites it. That state machine says what starts and ends each condition
  (control lost as defined there, ruling R-18; shutdown; a generation
  change), the grace at boot and after a generation change (so that the
  local default `{ action: safe, after: 0s }` does not defeat `at_start:
  hold` or `on_generation_change: hold`), that a change to MANUAL or any
  accepted request for the output ends the condition and cancels a pending
  repeat at once, how a write that was not confirmed is repeated (starting
  at `failsafe_retry`, with back-off, within `write_limits` and the budget
  of an `nvm` target, and stopping after a stated number of refusals), who
  owns `<output>.failsafe` and when it returns to 0, and which checks of a
  write request a fail-safe write passes. **Review by a person.** Repeating
  a fail-safe write is an exception to "a write is never retried", for
  fail-safe actions only (open: needs the product owner's ruling, R-11 of
  the driver interface contract).
- **A fail-safe write never waits for an audit record.** "No audit, no
  action" (reference 5.3, 8.2 step 8) is a rule for commands. A safe state on
  a failure is the configured answer to that failure, and the audit service
  may be down in the same failure. The write is made, then journalled
  best-effort: the record is posted, and a refused post is counted and
  logged. Open: needs the product owner's ruling (O-1), because it qualifies
  "no audit, no action" for fail-safe actions only.
- A crash of `scada-io` itself leaves its local outputs as the hardware
  leaves them; the mechanism that drives safe states after a crash is in
  Annex B.4.

The behaviour itself is built in phases 2 (`scada-io`, #107) and 5; phase 2
validates the keys.
## 8. Alarms

An alarm (reference 10.1) is given in the `alarms` list of a point, a bit or a
tag, or in `alarms/*.yaml` with the tag it watches.

```yaml
# alarms/ps1.yaml
priorities: [critical, high, medium, low]
max_shelve: { critical: 1h, high: 8h, medium: 24h, low: 7d }
classes: [process, equipment, system]
alarms:
  - { id: level_hh, tag: ps1.tank.level, type: high_high, limit: 4.8, deadband: 0.1,
      priority: critical, area: ps1, message: "tank level very high", guidance: "stop pumps P1 and P2" }
  - id: dry_run
    name: ps1.pump2.dry_run
    type: expression
    expr: ps1.pump2.running AND ps1.pump2.flow < 1.0
    on_delay: 30s
    priority: high
    area: ps1
    suppress_when: ps1.station.maintenance
```

Keys of the file:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `priorities` | list of identifiers, 1 to 8, most urgent first | `[critical, high, medium, low]` | Reference 10.1. At most one file of the tree may give it (CF-811) |
| `max_shelve` | map of priority to duration | `8h` for every priority | Reference 10.2. Same rule |
| `classes` | list of identifiers | `[process, system]` | Same rule |
| `alarms` | list | none | |

Keys of an alarm:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | identifier | the `type` | Unique per tag (CF-812) |
| `tag` | tag reference | the point or tag it is written in | Required in `alarms/*.yaml` except for `type: expression` |
| `name` | tag name | none | Required for `type: expression`: the base name of its state tags |
| `type` | `high_high`, `high`, `low`, `low_low`, `deviation`, `rate_of_change`, `state`, `bad_quality`, `expression` | required | Reference 10.1 |
| `limit` | number, or tag reference | required for the four limit types, `deviation`, `rate_of_change` | A tag reference makes the limit adjustable (reference 10.1) |
| `deadband` | number or percentage | 0 | Hysteresis on return to normal |
| `setpoint` | tag reference | required for `deviation` | Alarm when the absolute difference exceeds `limit` |
| `per` | duration | `1s` for `rate_of_change` | Rate is change per this time |
| `alarm_on` | value or list | required for `state` | Values, or enumeration names, that are the alarm state |
| `expr` | BOOL expression | required for `expression` | Section 5.6 |
| `on_delay`, `off_delay` | duration, `0s` to `1d` | `0s` | |
| `priority` | priority name | required | |
| `area` | area reference | the tag's area, else required | |
| `class` | class name | first class | |
| `latched` | bool | `false` | |
| `suppress_when` | BOOL expression | none | Suppressed by design while true |
| `message` | text | the tag name and type | What happened |
| `guidance` | text | none | What to do |

- Limits are checked against the type: `high_high` above `high`, `low` above
  `low_low`, where both are numbers (CF-813). An alarm on a STRING tag is
  CF-814, a limit alarm on a BOOL tag CF-815.
- Every alarm gets generated state tags owned by `scada-alarm` (section 12.5).
  They are retained, so acknowledgement and shelving survive a restart
  (reference 10.2), and refuse `retain_max_age` (section 5.2).
- Alarm limits and texts are not in the tag set: a change is applied live.
  Adding or removing an alarm adds or removes its state tags, which changes
  the tag set.
- Notification rules are phase 7 (`notifications.yaml`, reserved).

## 9. Control

### 9.1 Tasks

```yaml
# control/tasks.yaml
tasks:
  fast:   { period: 10ms,  priority: 90, rt_required: false, programs: [] }
  normal: { period: 100ms, priority: 50, programs: [ps1_level, ps1_seq] }
  slow:   { period: 1s,    priority: 20, programs: [reports] }
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `tasks.<name>.period` | duration, `10ms` to `1h` | required | Reference 7.1 |
| `tasks.<name>.priority` | 1 to 90 | 50 | Thread priority; in the real-time tier, the `SCHED_FIFO` priority. It is below `realtime.io_priority` of `system.yaml` (CF-906), and never 99, the level of the kernel's own per-CPU threads |
| `tasks.<name>.scan_group` | reserved | | Run once per scan of a synchronous scan group instead of on a timer (reference 6.2, 7.1). Reserved in both contracts until real-time protocol drivers are added by a later minor version (open: needs the product owner's ruling (O-4)) |
| `tasks.<name>.rt_required` | bool | `false` | Reference 4.3, 7.5 |
| `tasks.<name>.overrun_alarm_after` | 1 to 1,000 | 3 | Consecutive overruns that raise a system alarm (reference 7.2) |
| `tasks.<name>.budget` | 1,000 to 100,000,000 | 1,000,000 | Instruction budget per cycle, for each program and the virtual tags of the task (reference 7.4) |
| `tasks.<name>.programs` | list of program references | `[]` | The ordered list of programs (reference 7.1) |

- Without `control/tasks.yaml`, the tasks `fast` (10 ms), `normal` (100 ms)
  and `slow` (1 s) exist (reference 7.1). A file replaces them all.
- Every program is listed in exactly one task (CF-901). A program file that
  gives `task` must name that task (CF-902).
- Timing rules of reference 7.5: a task whose period is shorter than the poll
  period of any input it reads is warning CW-903; a task marked
  `rt_required` that reads a tag of anything but `scada-io` is CF-904
  (synchronous scan groups are reserved). The CPUs of real-time threads
  are `realtime.cpus` in `system.yaml`.

### 9.2 Function block programs

```yaml
# control/ps1_level.yaml
program: ps1_level
task: normal
blocks:
  - id: level_pid
    type: pid
    in:  { pv: ps1.tank.level, sp: ps1.tank.level_sp }
    params: { kp: 2.0, ti: 60s, td: 0s, out_min: 0, out_max: 100 }
    tunable: [kp, ti]
    on_bad_input: { action: hold, then_safe_after: 30s, safe_value: 0 }
  - id: speed_out
    type: analog_output
    in:  { value: level_pid.out, interlock: ps1.pump2.permissive }
    output: ps1.pump2.speed_cmd
```

| Key | Type | Meaning |
| --- | --- | --- |
| `program` | identifier | Equals the file name |
| `task` | task reference | Optional; must agree with `tasks.yaml` |
| `blocks` | list | Executed in data-flow order; where two orders are possible, in list order |
| `blocks[].id` | identifier | Unique in the program |
| `blocks[].type` | block type | A type registered in the block library (reference 7.3); unknown is CF-910 |
| `blocks[].in` | map of input pin to source | A source is a tag reference, `<block id>.<pin>`, a literal, or `{ tag, max_age }` / `{ from, prev: true }` |
| `blocks[].params` | map | Parameters of the block type, checked against its declaration (CF-911) |
| `blocks[].tunable` | list of parameter names | Parameters exposed as retained tags (reference 7.3, section 12.5) |
| `blocks[].on_bad_input` | `{ action: hold \| safe_value \| propagate, then_safe_after, safe_value }` | Reference 7.3; default from the block type (regulation and output blocks: `hold`) |
| `blocks[].out` | map of output pin to tag reference | Writes the pin to an internal tag of this task (CF-905) |
| `blocks[].output` | output reference | For output blocks: the output whose AUTO request the block writes |

- A source `a.b` is a block pin if `a` is a block id of the program, else a
  tag; if both exist the reference is CF-912.
- Every input pin a block type declares required is connected (CF-913); pin
  types match (BOOL, INT, REAL, TIME), with INT widened to REAL (CF-914).
- The block graph has no cycle (CF-915) except through a source marked
  `prev: true`, which reads the value of the previous cycle (reference 12.5,
  "unintended loops").
- An output is driven by at most one block of one program (CF-916), and only
  from the task that owns its AUTO request.
- The block catalogue (types, pins, parameters, defaults) is the block
  library's declaration, built in phase 6; the validator reads it from the
  library. Phase 2 validates the structure above and the block types the
  library declares at that time.

### 9.3 Structured-text programs

A file `control/<name>.st` holds one program:

```
PROGRAM ps1_seq
VAR
  start_delay : TON;
  step : INT := 0;
END_VAR
start_delay(IN := ps1.start_request AND ps1.pump2.permissive, PT := T#5s);
IF start_delay.Q AND step = 0 THEN
  ps1.pump2.speed_cmd := 60.0;
  step := 1;
ELSIF NOT ps1.start_request THEN
  ps1.pump2.speed_cmd := 0.0;
  step := 0;
END_IF;
END_PROGRAM
```

```
program     = "PROGRAM" identifier { var_block } { statement } "END_PROGRAM" ;
var_block   = "VAR" { var_decl } "END_VAR" ;
var_decl    = identifier ":" ( type | block_type ) [ ":=" literal ] ";" ;
type        = "BOOL" | "INT" | "REAL" | "TIME" ;
statement   = ( assignment | if_stmt | case_stmt | for_stmt | fb_call ) ";" ;
assignment  = target ":=" expression ;
target      = identifier | tag_ref ;
if_stmt     = "IF" expression "THEN" { statement }
              { "ELSIF" expression "THEN" { statement } }
              [ "ELSE" { statement } ] "END_IF" ;
case_stmt   = "CASE" expression "OF" { case_label { "," case_label } ":" { statement } }
              [ "ELSE" { statement } ] "END_CASE" ;
case_label  = integer | integer ".." integer ;
for_stmt    = "FOR" identifier ":=" integer "TO" integer [ "BY" integer ] "DO"
              { statement } "END_FOR" ;
fb_call     = identifier "(" [ identifier ":=" expression { "," identifier ":=" expression } ] ")" ;
```

`expression`, `literal` and `identifier` are those of section 5.6, and `tag_ref` is its `reference` naming a tag;
a block instance's outputs are read as `inst.Q` in expressions. Comments are
`(* ... *)` and `// ...` to the end of the line.

- No `WHILE`, `REPEAT`, pointers, dynamic memory, recursion or user types
  (reference 7.4). `FOR` bounds are integer literals, at most 10,000
  iterations (CF-921).
- A program may assign to an internal tag of its task, to a local variable,
  or to an output (its AUTO request). Anything else is CF-922.
- Local variables are not retained; a value that must survive a restart is an
  internal tag with `retain: true`.
- Programs are compiled to bytecode at validation; a syntax or type error
  blocks the commit with its file and line (CF-920, reference 7.4).

## 10. Validation rules and error codes

Each rule has a stable code and a fixed message. Placeholders in braces are
filled from the tree. Codes `CF-` are errors, which block a commit; codes
`CW-` are warnings. Decision D-77: the codes, their numbering by section, and
the rule that a code never changes meaning once 1.0 is cut.

The list covers reference 12.5 and the survey's rules. A new rule gets a new
code; a rule that is removed keeps its code unused. These are the codes of
every configuration refusal, also of those that need a protocol's
knowledge: `scada-conf` runs each protocol's `configure` (section 13.1),
whose refusals carry codes CF-650 to CF-699, listed in the protocol's annex.
The driver interface contract cites these codes; its own codes are for
runtime diagnostics only.

### 10.1 Files and syntax (CF-1xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-101 | A file at a path section 1 does not list, a reserved file, a file that is not a regular file or directory, or a path outside the path grammar | `{path} is not a file of the configuration tree` |
| CF-102 | A file name does not match its name key | `{path}: the file name must be {expected}.yaml` |
| CF-103 | `system.yaml` missing | `system.yaml is missing` |
| CF-110 | Outside the YAML subset | `{construct} is not allowed in configuration files` |
| CF-111 | Duplicate key | `duplicate key {key}` |
| CF-112 | Null or empty value | `{key} has no value` |
| CF-113 | A size limit exceeded | `{what} exceeds its limit of {limit}` |
| CF-114 | Not valid UTF-8, or a control character | `invalid character at byte {offset}` |

### 10.2 Schema (CF-2xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-201 | Unknown key | `unknown key {key} in {object}` |
| CF-202 | Missing required key | `{object} needs {key}` |
| CF-203 | Wrong type, or a scalar that matches no form of its type (section 1.2) | `{key} must be {type}` |
| CF-204 | Value out of range | `{key} must be between {min} and {max}` |
| CF-205 | Not one of the allowed values | `{key} must be one of {values}` |
| CF-206 | Bad duration, size or percentage | `{value} is not a valid {kind}` |
| CF-207 | Keys that exclude each other | `{key} cannot be given with {other}` |
| CF-208 | A key that needs another | `{key} needs {other}` |
| CF-210 | A reserved key or value | `{key} is reserved and not available in this version` |
| CF-211 | `format` not supported | `format {n} is not supported; this version reads format {m}` |
| CF-212 | A name that breaks its grammar or length (section 2.1) | `{name} is not a valid {kind} name` |
| CF-213 | A plain value where a secret is required | `{key} must be a secret reference` |
| CF-214 | A secret reference (phase 2) | `secrets are not available in this version` |

### 10.3 Names and references (CF-3xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-301 | A user tag name in `sys` | `tag names starting with sys. are reserved` |
| CF-302 | A tag name used twice | `tag {name} is defined twice; first at {file}:{line}` |
| CF-303 | A device, channel, template, policy, area, program, task, enumeration or interlock name used twice | `{kind} {name} is defined twice; first at {file}:{line}` |
| CF-310 | A reference that does not resolve | `{kind} {name} does not exist` |
| CF-311 | A pattern that matches nothing | `pattern {pattern} matches no tag` |
| CF-312 | A reference to a tag of the wrong type | `{name} must be a {type} tag` |

### 10.4 Templates and instances (CF-4xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-401 | `extends` cycle, or deeper than 4 | `template {name}: extends {problem}` |
| CF-402 | A tag name longer than 127 bytes after the prefix (#119) | `tag name {name} is {n} bytes, the limit is 127` |
| CF-403 | A generated tag name longer than 127 bytes | `generated tag {name} is {n} bytes, the limit is 127` |
| CF-410 | A configuration expression that fails | `expression {expr}: {reason}` |
| CF-411 | A parameter without value | `parameter {name} needs a value` |
| CF-412 | A parameter value of the wrong type or range | `parameter {name}: {reason}` |
| CF-413 | `disable`, `only` or `overrides` of an unknown point | `point {name} does not exist in template {template}` |
| CF-414 | More than 65,536 points after expansion | `template {name} expands to {n} points, the limit is 65536` |
| CF-415 | A child changes a parameter's type | `parameter {name} cannot change its type` |
| CF-416 | Two points of one name in one level, or an instance point that exists | `point {name} is defined twice` |
| CF-417 | A point without `address` in a protocol template after expansion | `point {name} has no address` |
| CF-419 | `when` in `defaults` or `overrides` | `when cannot be given in {key}` |
| CF-420 | `protocol` of the template does not match the channel's | `template {name} is for {protocol}, the channel is {channel_protocol}` |
| CF-421 | Base and protocol template mixed wrongly | `template {name}: {reason}` |
| CF-422 | A point merged from two `repeat` blocks that bind one variable to two values (section 4.5, step 3.2) | `point {name}: variable {var} is {a} in {template_a} and {b} in {template_b}` |

### 10.5 Tags and expressions (CF-5xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-504 | A `value_type` that cannot hold the raw value | `{name}: value_type {type} cannot hold {raw_type}` |
| CF-505 | A percentage without `range` | `{name}: a percentage needs range` |
| CF-506 | `retain_now` or `retain_max_age` without `retain` | `{key} needs retain: true` |
| CF-507 | A STRING tag retained | `string tags cannot be retained` |
| CF-508 | `retain_max_age` on a mode, authority, alarm state or counter tag (#89) | `retain_max_age is not allowed on {kind} tags` |
| CF-509 | `retain_now` on a point tag or virtual tag | `retain_now is allowed on internal and manual tags only` |
| CW-510 | `retain_max_age` before tag database 1.1 | `retain_max_age has no effect until the tag database supports it (#89)` |
| CF-520 | Expression syntax | `expression: {reason} at column {col}` |
| CF-521 | An ambiguous `.quality` or `.age` | `{ref} is both a tag and an attribute; use quality() or age()` |
| CF-522 | A STRING tag in an expression | `string tag {name} cannot be used in an expression` |
| CF-523 | A type mismatch | `expression: {reason}` |
| CF-524 | A time function window out of range | `{function}: the window must be 1 to 10000 cycles of task {task}` |
| CF-525 | A cycle of virtual tags | `virtual tags form a cycle: {names}` |
| CF-526 | An expression too long or too deep | `expression exceeds {limit}` |
| CF-527 | A virtual tag whose result type does not match `value_type` | `{name}: the expression gives {type}` |
| CF-528 | An integer literal above 2^63 − 1, or a real literal that underflows to zero or overflows (section 5.6) | `literal {text} is out of range` |
| CF-529 | A call of a name that is not a function (section 5.6) | `{name} is not a function` |
| CF-516 | An integer scaling that cannot fit the value type for the raw type's whole range | `{name}: raw x gain + offset can exceed {type}` |

### 10.6 Drivers, devices and points (CF-6xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-601 | Two devices on one channel with the same address, or overlapping ranges, where the protocol forbids it (reference 12.5; Annex A.3) | `devices {a} and {b} overlap at {address}` |
| CF-602 | Two points of one device that overlap where the protocol forbids it | `points {a} and {b} overlap at {address}` |
| CF-603 | An address not valid for the protocol | `{name}: {reason}` |
| CF-604 | An unknown poll group | `poll group {name} does not exist on channel {channel}` |
| CF-605 | More devices on a channel than `limits.max_devices_per_channel` | `channel {name} has {n} devices, the limit is {limit}` |
| CW-606 | Pacing cannot serve a poll group. Open: needs the product owner's ruling (X-13): a warning in this draft | `channel {name}: poll group {group} needs about {needed} per cycle at the configured pacing, its period is {period}` |
| CW-607 | The slowest poll period or `backoff_max` is longer than the device's `idle_timeout` and `keepalive` is off | `device {name}: the connection may be closed between polls; set keepalive` |
| CW-608 | The requests of a poll group, with their line time at the channel's baud rate (section 4.11) and the pacing, do not fit its period | `channel {name}: poll group {group} needs {needed} on the line, its period is {period}` |
| CF-610 | Points decoded from one side-effect range in different poll groups; a side-effect range used by an identity check or a write; a side-effect or `exclusive` range that overlaps another range kind | `range {range} of device {device}: {reason}` |
| CF-611 | `max_age` below `update_period` | `point {name}: max_age is shorter than the device's update period` |
| CF-612 | A companion (exponent, quality, time, health) in another poll group than the point that uses it, or not read by the same device | `point {name}: {companion} must be in poll group {group}` |
| CF-613 | A double point without a needed `states` map | `point {name}: states must be given` |
| CF-614 | A `wo` point in a poll group or with alarms or logging | `point {name} is write-only and cannot be read` |
| CF-615 | A device function not in the device's supported set (Annex A) | `point {name} needs function {fn}, which device {device} does not support` |
| CF-616 | An identity check on a point that is not read | `identity check {n} of {device}: point {point} is not read` |
| CF-618 | A bit, a field or `bits` in a template without `bit_order` | `template {name} uses bits and needs bit_order` |
| CF-619 | A writable point without `storage` | `point {name} is writable and needs storage: volatile or nvm` |
| CF-620 | Two channels on one serial device path | `channels {a} and {b} use {device}` |
| CW-621 | Two channels to one host and port | `channels {a} and {b} both connect to {endpoint}; most devices allow one client` |
| CF-622 | A key or value of a protocol schema refused by its rules (Annex A.1: `data_bits` 7 on RTU, a loopback, multicast or broadcast address) | `{key}: {reason}` |
| CF-640 | `keep_last` (in `on_invalid` or an `action`) on a point without `max_age` | `point {name}: keep_last needs max_age` |
| CF-650 to CF-699 | Refusals of a protocol's `configure` and `plan` (section 13.1), each listed in the protocol's annex | as listed |

### 10.7 Areas and outputs (CF-7xx)

| Code | Rule | Message |
| --- | --- | --- |
| CW-701 | An area used by nothing | `area {name} is not used` |
| CF-702 | An output target that is not a writable point | `output {name}: target {tag} is not a writable point` |
| CF-703 | A target in two outputs | `tag {tag} is the target of outputs {a} and {b}` |
| CF-704 | `select_timeout` longer than the device's select window | `output {name}: select_timeout {t} exceeds the device's select window {w}` |
| CF-705 | A dangerous output driven by a program, or with `refresh`, a `safe_state`, `on_shutdown` or `on_control_loss` other than `hold` | `output {name} is dangerous: {reason}` |
| CF-706 | An `nvm` target driven by a program, or with `refresh` | `output {name} writes non-volatile memory and cannot be written cyclically` |
| CF-707 | An off value for an `on_only` output | `output {name} is on-only` |
| CF-708 | A target address the protocol cannot write | `output {name}: the protocol cannot write {address}` |
| CF-709 | No `feedback` and no `no_feedback` | `output {name} needs feedback or no_feedback: true` |
| CF-710 | No `safe_state` | `output {name} needs a safe_state` |
| CF-711 | A switching output without `interlocks`. Open: needs the product owner's ruling (O-21) | `output {name} needs interlocks (an empty list if none applies)` |
| CW-712 | A switching output with `interlocks: []` | `output {name} has no interlock` |
| CF-713 | A value (limit, safe value, release) outside the target's type | `output {name}: {key} does not fit the target's type` |
| CF-714 | `refresh` on a kind other than `setpoint`, on an `nvm` target, with `sbo`; `release` in `on_shutdown` or `on_control_loss` without `limits.release` | `output {name}: {reason}` |
| CF-715 | A feedback tag of the wrong kind for the output's kind | `output {name}: feedback must be {kind}` |
| CF-716 | `native_select: at_operate` with a device select window below twice the device's `timeout` | `output {name}: the device's select window {w} is too short for at_operate` |
| CF-717 | An output with `refresh` whose program's output block also refreshes it (reference 7.2, step 3) | `output {name} is refreshed twice` |
| CF-718 | `local_blocks_auto` without `local_panel`, or a `local_panel.input` that is not a BOOL tag read by a driver | `area {name}: {reason}` |
| CF-719 | A `step`, `pulse` or `trigger` output whose `safe_state` or fail-safe key is not `hold` | `output {name}: a {kind} output has no safe action; use hold` |
| CF-720 | A `duration` on a `pulse` output (a pulse timed by the unit) | `output {name}: the device must time the pulse` |
| CF-723 | `release` in a fail-safe key of an output that is not a `setpoint` (a release value has a meaning only for a setpoint, driver interface contract 8.3) | `output {name}: release is only for setpoints` |
| CF-722 | `safe` in `on_shutdown`, `on_control_loss` or `on_generation_change` while `safe_state` is `hold` | `output {name}: action safe needs a safe_state other than hold` |
| CW-721 | A `single` or `double` output on a protocol driver, not `dangerous`, that gives neither `on_control_loss` nor `on_shutdown`, so both are `hold` by default (section 7.2) | `output {name} keeps its state when control stops; set on_control_loss and on_shutdown if it must not` |

### 10.8 Alarms and logging (CF-8xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-801 | Tier intervals not increasing or not multiples | `aggregate tier {name}: {reason}` |
| CF-802 | Inline logging settings without `mode` or `raw_retention` | `logging of {name} needs {key}` |
| CF-803 | A STRING tag logged other than `on_change` | `string tag {name} can be logged on change only` |
| CW-804 | More than 32 retention classes | `{n} retention classes; more than 32 increases the number of open files` |
| CW-805 | A logging interval finer than the tag's refresh | `{name}: interval {i} is finer than the tag's refresh {r}` |
| CF-811 | `priorities`, `max_shelve` or `classes` in two files | `{key} is given in {a} and {b}` |
| CF-812 | Two alarms of one id on one tag | `tag {name} has two alarms with id {id}` |
| CF-813 | Limits in the wrong order | `tag {name}: {a} must be above {b}` |
| CF-814 | An alarm on a STRING tag | `string tag {name} cannot have alarms` |
| CF-815 | A limit alarm on a BOOL tag | `BOOL tag {name} cannot have a {type} alarm` |

### 10.9 Control (CF-9xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-901 | A program in no task or in two | `program {name} must be in exactly one task` |
| CF-902 | A program's `task` disagrees with `tasks.yaml` | `program {name}: task {a} disagrees with tasks.yaml ({b})` |
| CW-903 | A task faster than the poll period of an input (reference 7.5) | `task {task} runs every {p} but reads {tag}, polled every {q}` |
| CF-904 | An `rt_required` task with an input outside `scada-io` or a synchronous scan group (reference 7.5) | `task {task} is rt_required and reads {tag} from {source}` |
| CF-905 | An internal tag written by two tasks or by a task other than its own | `tag {name} is written by {a} and {b}` |
| CF-906 | A task priority at or above `realtime.io_priority` | `task {name}: priority {p} must be below {io}` |
| CF-910 | An unknown block type | `block type {type} does not exist` |
| CF-911 | A block parameter unknown, missing or out of range | `block {id}: {reason}` |
| CF-912 | A source that is both a pin and a tag | `{ref} is both a block pin and a tag` |
| CF-913 | A required input pin not connected | `block {id}: input {pin} is not connected` |
| CF-914 | Pin types do not match | `block {id}: input {pin} needs {type}, {source} is {other}` |
| CF-915 | A loop in the block graph | `blocks form a loop: {ids}; mark one source prev: true if intended` |
| CF-916 | An output driven twice | `output {name} is driven by {a} and {b}` |
| CF-920 | A structured-text error | `{reason}` |
| CF-921 | A `FOR` loop with non-literal bounds or more than 10,000 iterations | `FOR loop: {reason}` |
| CF-922 | An assignment to something the program may not write | `{target} cannot be written by program {name}` |

### 10.10 Tag set and limits (CF-10xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-1001 | More tags than `limits.max_tags` | `{n} tags, the limit is {limit}` |
| CF-1002 | More devices than the limits | `{n} devices, the limit is {limit}` |
| CF-1003 | An owner with more than 524,288 tags, or a ring capacity below twice its tags (tag database contract 5, 14) | `owner {name}: {reason}` |
| CF-1004 | A queue or owner name longer than 31 bytes | `name {name} is longer than 31 bytes` |
| CF-1006 | Two channels or two tasks with the same short name (section 12.3) | `{kind} {a} and {b} have the same short name {c}; rename one` |
| CF-1007 | More than 4,096 queues | `{n} queues, the limit is 4096` |
| CF-1008 | More than 1,023 owners | `{n} owners, the limit is 1023` |
| CF-1009 | More than 126 command queues consumed by one driver process (one wait set of 127 sources holds them, tag database contract 8.4) | `driver {name} consumes {n} command queues, the limit is 126` |
| CF-1005 | A service user that does not exist on the unit, uid 0, or two processes with one uid | `process {name}: {reason}` |
| CF-1010 | `heartbeat_check_ms` above 400 with a `retain_now` tag (D-62, #96) | `heartbeat_check_ms must be at most 400 in a tag set with a retain_now tag` |
| CF-1011 | Any other refusal of `scada::tag::validate` | `tag set: {reason}` |
| CF-1012 | The check mode of a protocol's driver (section 13.1) crashed, exceeded its limits or could not run | `protocol {name}: the check could not run: {reason}` |
| CW-1013 | Offline validation without the driver executable of a protocol: stage 7 not run; the result cannot be committed | `protocol {name}: not checked (driver not installed)` |
| CF-1016 | The expanded file of one process would exceed the limits of section 13.3 (size, nodes, depth) | `process {name}: the expanded configuration has {n} {unit}, the limit is {limit}` |
| CF-1014 | A task named `cmd` (its queues would be those of `scada-cmd`, section 12.3) | `task name cmd is reserved` |
| CW-1015 | A process needs more threads, descriptors or locked memory than the limits of its installed unit (`/usr/share/scada/unit-limits.json`, installed with the units by #105). Nothing writes a unit file at run time | `process {name} needs {n} {resource}; its unit allows {limit}` |

A refusal of the tag-set builder names the configuration line that caused it:
the tag's definition, the channel file of an owner, `system.yaml` for data
plane settings.

## 11. Versioning and migration

Decision D-80.

- The **tree format** is the integer `format` of `system.yaml`, 1 for this
  contract; a tree without it is of format 1. A software version reads
  exactly one format; a tree of another format is CF-211.
- A change that adds an optional key or value, or accepts a reserved one, is
  a **contract minor version** (0.x now; 1.1, 1.2 after 1.0) and keeps the
  format. A tree that uses the addition is refused by older software as an
  unknown or reserved key, never misread.
- A change that makes a valid tree invalid, or changes what a valid tree
  means, is a **new format**, N + 1. It comes with a migration from N to N + 1
  in `libscada-conf`: a pure function from a tree of format N to a tree of
  format N + 1, which rewrites only the lines it must change and keeps
  comments elsewhere.
- After a software update (reference 14.2), `scada-conf` migrates the active
  tree step by step to its format and commits the result as a new version,
  authored by the update, with the migration steps as its comment, then
  validates it. If validation fails, the update's health check fails and the
  update rolls back (reference 14.2). `scadactl config migrate` stages a
  migration of a tree, for example an imported archive, for review like any
  edit (reference 12.4, 12.7).
- A protocol schema (Annex A) is versioned with the tree format: a change to
  it follows the same rules.
- History is never rewritten: older versions in the history keep their
  format, and a rollback to one is migrated again like an import.
- While the contract is a draft (0.x), format 1 may change without a
  migration.

## 12. Tag set and `config_hash`

This section defines the input of the tag database: the tag-set definition
(`scada::tag::TagSet`, tag database contract sections 2, 5 and 14, D-26), which
`scada-conf` computes from a valid tree (#101). `config_hash` is computed by
`libscada-tag` from the tag set, exactly as tag database contract 5 and the
canonical encoding of `libs/scada-tag/README.md` say. This contract adds no
field to the tag set and no step to the hash.

### 12.1 What the tag set holds

| Part | From |
| --- | --- |
| `heartbeat_check_ms` | `system.yaml`, `data_plane.heartbeat_check_ms` |
| Processes: name, uid, heartbeat timeout, REALTIME | Section 12.2 |
| Owners: name, process, change ring capacity | Section 12.3 |
| Queues: name, producer, consumer, capacity | Section 12.3 |
| Tags: name, owner, type, flags RETAIN, WRITABLE_BY_COMMAND, RETAIN_NOW | Sections 12.4 and 12.5 |

Nothing else of the tree is in the tag set: addresses, scaling, poll groups,
alarm limits, logging, program logic and `retain_max_age` are not, so a change
to them leaves `config_hash` unchanged and is applied live (reference 5.5).

### 12.2 Processes

Decision D-73.

| Process | Service and user | In the tag set | REALTIME |
| --- | --- | --- | --- |
| `tagd` | `scada-tagd` | Always | No |
| `control` | `scada-control` | Always | Yes |
| `cmd` | `scada-cmd` | Always | No |
| `alarm` | `scada-alarm` | Always | No |
| `hist` | `scada-hist` | Always | No |
| `api` | `scada-api` | Always | No |
| `audit` | `scada-audit` | Always | No |
| `io` | `scada-io` | When `drivers/io/` has a channel | Yes |
| `drv-<protocol>` | `scada-drv-<protocol>` | When `drivers/<protocol>/` has a channel | No |

- The process name is the service name without `scada-`; the service user is
  the service name (`docs/install.md`, one system user per service). The
  install tree creates every service user of this table, also of services
  that are built in later phases, so that a tree can be validated on a unit
  from phase 2 on (#102).
- **uid.** The tag set holds numeric uids (D-26). `scada-conf` looks up the
  uid of each service user on the unit when it builds the tag set. A user
  that does not exist, uid 0, or two processes with one uid is CF-1005 (the
  D-26 note for phase 2). The uid is in the canonical encoding, so
  `config_hash` depends on the unit as well as the tree (section 12.8).
  Tests give the lookup a fixed table of users, so that a tree's hash is
  pinned in a test (#101). Open: needs the product owner's ruling (O-6).
- **Validation without a unit.** `scadactl config validate --offline` (and
  the same library call in the pipeline) runs every stage but the uid
  lookup, and reports no `config_hash`. Engineering machines use it.
- **A changed uid** makes a new version (section 13.2). Until that version
  is applied, a service whose uid changed cannot attach to the running
  generation (the tag database checks the uid): it waits, watching `active`
  as an idle driver does (13.7), and does not exit, so that it does not run
  into the restart limit of reference 14.4. A changed uid of `scada-conf`
  itself leaves it unable to write its store; it logs that and exits with
  a failure, which the installer (#105) must prevent by keeping the uid.
- The heartbeat timeout is `data_plane.processes.<process>.heartbeat_timeout_ms`,
  default 3,000 ms, and 500 ms for a REALTIME process (D-26).
- **Real time in phase 2.** Only `control` and `io` are REALTIME. Modbus
  workers run under the normal scheduler. Real-time protocol drivers and
  synchronous scan groups are a later minor version of this contract and of
  the driver interface contract together (open: needs the product owner's
  ruling (O-4); at common serial rates a 10 ms Modbus scan is not reachable:
  15 bytes take 8.6 ms at 19,200 bit/s).
- `scada-conf` is not in the tag set: it does not attach.

### 12.3 Owners and queues

Decision D-75.

**Short names.** Owner and queue names are at most 31 bytes (tag database
contract 1, 3), and the part of a queue name after its prefix is free text
there. Channel and task names (identifiers of up to 31 bytes) are therefore
shortened where they appear in owner and queue names:

```
S(kind, name, n) = name                                  if name has at most n bytes
                 = first (n - 9) bytes of name, "-", the first 8 hexadecimal digits
                   (lower case) of SHA-256(kind + ":" + name)          otherwise
C(channel) = S("channel", channel, 15)
T(task)    = S("task", task, 12)
```

- A short name depends on its own name only. Adding, removing, reordering or
  renaming one channel or task changes the owner and queue names of that
  channel or task alone.
- An identifier has no `-`, so a name kept whole and a shortened one cannot
  be equal. Two shortened names that are equal are CF-1006 ("channels {a}
  and {b} have the same short name {c}; rename one"); with 32 bits and the
  readable prefix the chance is small, and a collision is an error, never a
  silent merge.
- Protocol names are at most 10 bytes, so `drv-<protocol>.<C>` is at most
  30 bytes.
- `scada-conf` writes, with every committed version, the map from every
  generated name to what it stands for, with the version's `config_hash`
  (section 13.2). `scadactl` shows both, for example
  `drv-modbus.inverter-3f9a1c22 (channel inverters_field_north)`;
  `scada-tagd` and the tag database know only the generated names. Since the
  map is kept per version, older log records stay readable after a rename.

**Owners.** One owner is one writing thread (tag database contract 2, 13.2).
Every attached process has an owner named like the process, for the thread
that writes its process-level tags `sys.proc.<process>.*` (among them the
apply report of section 13.5); more owners follow its threads:

| Owner | Process | Thread | Writes |
| --- | --- | --- | --- |
| `<process>` | every process but `tagd` | Main thread | `sys.proc.<process>.*` |
| `control.<T(task)>` | `control` | The task's thread | The task's virtual, internal and interlock tags, AUTO request tags, tunable parameter tags |
| `cmd` | `cmd` | Its writing thread | Also manual tags, MANUAL request tags, mode and status tags of outputs, authority tags of areas |
| `alarm` | `alarm` | Its writing thread | Also alarm state tags |
| `io.scan` | `io` | The scan thread (reference 6.5) | The tags of every `scada-io` channel and their `sys.drv.*` and `sys.dev.*` system tags, and `<output>.failsafe` of local outputs |
| `drv-<protocol>.<C(channel)>` | `drv-<protocol>` | The channel's worker | Every tag of the channel's devices, their `sys.dev.<device>.*` tags, the channel's `sys.drv.<channel>.*` tags, and `<output>.refresh` and `<output>.failsafe` of outputs whose target is on the channel |

- An owner without tags is not created, except the process owner, which
  always has its `sys.proc` tags.
- **Ring capacity.** Each owner's change ring is sized from its own tag
  count: the next power of two of at least four times its tags, and at least
  1,024 entries (tag database contract 5, 8.3: at least twice), at most
  1,048,576 (CF-1003). A channel's `change_ring_capacity` overrides it. A
  channel of 100 tags then takes 32 KiB, not the 2 MiB of the tag database's
  default; 300 such channels take about 10 MiB, which keeps reference 5.8
  ("under 64 MB") (D-76).

**Queues**, each with the capacity of section 3. `<S>` is `cmd` for
`scada-cmd` and `T(task)` for a control task. A task named `cmd` (in any
case) is CF-1014, so that its queues cannot be those of `scada-cmd`:

| Queue | Producer, consumer | Exists when |
| --- | --- | --- |
| `cmd.<protocol>.cmd`, `result.<protocol>.cmd` | `cmd`, `drv-<protocol>` | The protocol has a channel with a point (operator writes and `READ_NOW`) |
| `cmd.<protocol>.<T(task)>`, `result.<protocol>.<T(task)>` | `control`, `drv-<protocol>` | A program of the task drives an output whose target is on a channel of the protocol |
| `cmd.io.<S>`, `result.io.<S>` | `cmd` or `control`, and `io` | As above, for `scada-io` |
| `events.cmd` | `cmd`, `audit` | Always |
| `events.control.<T(task)>` | `control`, `audit` | A program of the task drives an output (no audit, no action applies to AUTO writes, tag database contract 9.3) |
| `events.alarm` | `alarm`, `audit` | Always |
| `events.drv-<protocol>`, `events.io` | the driver process (its main thread), `audit` | From phase 5: the process has an output whose `on_shutdown`, `on_control_loss` or `on_generation_change` is not `hold`, or an output with `refresh`; it journals its own fail-safe writes after them (section 7.4) and the end of a refresh (driver interface contract) |

- Queues are per driver process and source, as reference 5.1 names them
  (`cmd.<driver>.<source>`). Inside a driver, one command thread consumes
  them and routes each request by its target handle to the channel's worker,
  whose order queue of `max_pending_writes` gives the isolation between
  channels: a full order queue answers `BUSY` (driver interface contract).
  This keeps the queue count small (two per source and driver, not per
  channel) and within one wait set of 127 sources (tag database contract
  8.4).
- The longest name is `result.<protocol>.<T>`: 7 + 10 + 1 + 12 = 30 bytes.
- Every queue has one producing thread: the main thread of a driver posts
  its event records, and its workers hand them to it.
- The consumer of the event queues is provisional: which service journals
  which record, and whether an AUTO write needs one record per request or
  per change of state, is decided with the command and audit services
  (phases 4 and 5, #83, D-66 g). A change of consumer is a tag set change and
  a minor version of this contract.
- A note for phase 3: a consumer of every channel's change ring
  (`scada-hist`, `scada-alarm`) meets the same 127-source limit of a wait
  set, and needs one waiter per 127 rings.

### 12.4 Tags from the tree

| Tag | Owner | Type | Flags |
| --- | --- | --- | --- |
| Point tag `<device>.<point>` | The channel's owner | `value_type` (section 4.7) | RETAIN if `retain`; WRITABLE_BY_COMMAND if the target of an output |
| Bit tag of a point | The channel's owner | BOOL | RETAIN if its `retain` |
| Internal tag | `control.<T(task)>` | `value_type` | RETAIN, RETAIN_NOW as given |
| Manual tag | `cmd` | `value_type` | RETAIN (default true), RETAIN_NOW as given |
| Virtual tag | `control.<T(task)>` | From `expr` | RETAIN if `retain` |

### 12.5 Generated tags

Decision D-79. Every generated tag name is checked against the 127-byte limit
(CF-403).

| Tag | Owner | Type | Flags | Values |
| --- | --- | --- | --- | --- |
| `<output>.mode` | `cmd` | INT64 | RETAIN, RETAIN_NOW | 1 AUTO, 2 MANUAL, 3 REMOTE (phase 11) |
| `<output>.status` | `cmd` | INT64 | none | 0 IDLE, 1 SELECTED, 2 EXECUTING, 3 OK, 4 FAILED, 5 BLOCKED (reference 8.1), 6 ACCEPTED_UNVERIFIED (section 7.2) |
| `<output>.req_manual` | `cmd` | the target's type | RETAIN | The value last requested in MANUAL |
| `<output>.req_auto` | `control.<T(task)>` | the target's type | none | The value last requested by logic |
| `<output>.refresh` | the target's channel owner | INT64 | none | Outputs with `refresh` only: 0 none, 1 refreshing, 2 ended without a new request (section 7.2) |
| `<output>.failsafe` | the thread that writes the target: the channel owner `drv-<protocol>.<C>` for a remote output, `io.scan` for a local one | INT64 | none | Outputs with a `safe` or `release` fail-safe key: 0 no fail-safe action, 1 the action was written and answered OK, 2 the action is due but not confirmed (it may not have reached the device; being repeated, or stopped after refusals). It returns to 0 when the condition ends (driver interface contract 8.13.3) |
| `<output>.device_selected` | the target's channel owner | BOOL | none | Only for a target whose protocol declares a native select (none in phase 2): the device holds a selection |
| `sys.area.<area>.authority` | `cmd` | INT64 | RETAIN, RETAIN_NOW | 1 LOCAL_PANEL, 2 CLI, 3 WEB, 4 API, 5 MASTER (phase 11) |
| Interlock `<name>` | `control.<T(task)>` | BOOL | none | Section 7.3 |
| `<tag>.alarm.<id>` | `alarm` | INT64 | RETAIN | 0 normal, 1 unacknowledged, 2 acknowledged, 3 returned unacknowledged, 4 latched unacknowledged, 5 latched acknowledged, 6 shelved, 7 suppressed, 8 out of service (reference 10.2) |
| `<tag>.alarm.<id>.shelved_until` | `alarm` | INT64 (ns UTC, 0 if not shelved) | RETAIN | |
| `ctl.<program>.<block>.<param>` | `control.<T(task)>` | The parameter's type | RETAIN | Tunable parameters (reference 7.3) |
| `sys.proc.<process>.config_id`, `sys.proc.<process>.config_state` | `<process>` | UINT64, INT64 | none | The apply report (section 13.5, 13.6): `config_id` is the first 64 bits of the `config_id` the state refers to; `config_state` 0 loading, 1 applied, 2 refused, 3 running the old configuration while waiting for a generation change. A state counts only together with its `config_id` |
| `sys.drv.<channel>.<name>`, `sys.dev.<device>.<name>` | the channel's owner (`io.scan` for `scada-io`) | As the driver interface contract lists | none | Per channel, per poll group of a channel, and per device (reference 6.3) |
| `sys.proc.<process>.<name>` of a driver | `<process>` | As the driver interface contract lists | none | Per driver process |

- For an expression alarm, `<tag>` is its `name`.
- The output's target is the point tag that write requests name; the
  feedback tag is a point tag of the device (reference 8.1).
- Driver system tags are listed, with their types, in the driver interface
  contract; `scada-conf` builds them by its rule: one set per process, per
  channel, per poll group of a channel (`period.<group>`), and per device,
  with the identity tags only for a device with `identify`. None is
  retained. Phase 2 has no scan-group tag (scan groups are reserved). The
  system tags of the services themselves (task cycle times, the real-time
  tier) are added with those services (phases 4 to 7), each a minor version
  of this contract.
- Adding or removing an alarm, an output or a tunable parameter adds or
  removes generated tags, and so restarts the data plane (section 12.7).

### 12.6 Agreement with tag database contract 1.0

- Names: tag names of at most 127 bytes of `[a-z0-9_.-]`, owner, process and
  queue names of at most 31 bytes, no owner name starting with `.`
  (contract 1). The grammar of section 2.1 is a subset.
- Queues: every `cmd.` queue has its `result.` queue (D-52); producer and
  consumer are different processes; every queue has one producing thread;
  capacities are powers of two up to 2,147,483,648 (contract 5).
- Owners: every owner is one writing thread (contract 2, 13.2); no thread
  writes the tags of two owners and no owner is written by two threads.
- Flags: `retain_now` only with `retain`; no STRING tag RETAIN or
  WRITABLE_BY_COMMAND (contract 5, 9.4, 12).
- `heartbeat_check_ms` at most 400 with a RETAIN_NOW tag (contract 12, D-62).
- Limits of contract 14: at most 1,000,000 tags, 1,023 owners (CF-1008),
  524,288 tags per owner, 1,024 processes, 4,096 queues (CF-1007); ring
  capacity 1,024 to 1,048,576 and at least twice the owner's tags (CF-1003).
- `scada-conf` calls `scada::tag::validate` on the tag set it built, and maps a
  refusal to the configuration line (CF-1011, #101). A check of this section
  that `scada-conf` already makes gives the more specific code first.

### 12.7 What changes `config_hash`

Any change to a part of section 12.1: adding, removing, renaming or retyping a
tag (also by a template change that reaches every instance, reference 12.2,
or by a new alarm, output or tunable parameter); moving a device to another
channel or a tag to another task (a new owner; retained values follow the
moved-tag rules of tag database contract 12); changing `retain` or
`retain_now`; a new or removed queue; a capacity; a heartbeat setting; a
service user's uid. Such a commit restarts the data plane (reference 5.5), and
the change workflow says so before the user confirms (reference 12.4, step 5).

The tag set is a pure function of the tree and the uid table. The order of
files, of entries in a file, and of keys in a map has no effect (the library
sorts, D-26), so `config_hash` is the same on x86-64 and ARM64 and after a
file is renamed or its entries reordered (#101).

### 12.8 `config_hash` across units, and `config_id`

What this means for an operator: **the same configuration tree gives
different `config_hash` values on two units whose service users have
different uids.** Users are created at installation (`systemd-sysusers`), so
two units installed differently, or a replacement unit, can have other uids.
`config_hash` says only "this generation of the data plane on this unit was
built from this tag set"; `scada-tagd` uses it to decide whether a restart can
reuse a generation, on the same unit (tag database contract 11.1). Two units
showing different `config_hash` values may run the same tree.

Where configurations are compared across units, `config_hash` is not used:

| Place | What it compares | Identity used |
| --- | --- | --- |
| Upstream sync (reference 9.6, phase 10) | Which configuration a historian partition or journal batch was recorded under | `config_id` below, with the unit id |
| Backup, restore and cloning (reference 12.7) | Whether an imported archive is the tree it claims to be; which version is active on a replacement unit | The archive's signature and the version id it carries (reference 12.7), and `config_id`. The replacement unit computes its own `config_hash` after the import |
| Node link manifest (reference 17.3, phase 11) | Whether the master's imported tag definitions still match the slave's exports | The manifest hash of the node link contract, over the exported tags (names, types, units), which holds no uid. That contract defines it |
| `scadactl`, the system page | Which configuration a unit runs | `config_id` and the version id; `config_hash` is shown as a data-plane detail |

**`config_id`** is the unit-independent identity of a tree: the SHA-256 of the
encoding `text "scada-config-tree", u32 1 (encoding version)`, then for each
file of the tree in byte order of its path, `text path, u64 length, bytes`
(integers little-endian, text as a 32-bit length and its bytes, as in the
canonical encoding of the tag set).

- Paths are relative to the root of the tree, with `/` as separator, sorted
  as bytes.
- It covers every byte of every file of the tree (section 1), so a change of
  a comment changes it, and so does the same text saved with other line
  ends (CRLF and LF are both allowed): it identifies a tree, not its
  meaning. Archives and the version store keep files byte for byte.
- File modes, owners and times are not covered; links cannot occur
  (section 1). The files `scada-conf` writes beside a tree (section 13.2)
  are not covered.
- `scada-conf` stores it with each committed version (section 13.2).
- Open: needs the product owner's ruling (O-6), together with the
  unit-dependent `config_hash`.

## 13. Apply: how a committed tree reaches the services

Decision D-100. **Review by a person** (restart behaviour). This section is
owned here and is the one text of the apply; the driver interface contract
cites it and adds only what a driver does inside a step. It fixes where
versions live, how one becomes active, who starts a generation change, what
each service does at each step and after a power cut, and what the apply
reports. #102 builds it.

### 13.1 Validation with the protocols

`scada-conf` does not link the protocol libraries (D-81: a protocol is
linked into its own driver executable only; a later protocol may carry a
licence or a fault that must not reach `scada-conf`). Stage 7 of validation
(section 1.4) instead runs, for each protocol that has a channel, that
protocol's driver executable in its check mode:

```
scada-drv-<protocol> --check <expanded-file>      (and scada-io --check)
```

- The check mode loads the expanded file (13.3) with the same loader the
  driver uses at run time, runs the protocol's `configure` and `plan` on
  every channel, writes one diagnostic per line to standard output (code,
  file, line, column, message; the format of `scadactl config validate
  --json`), and exits 0 (accepted), 1 (refused) or 2 (it could not run). It
  attaches to nothing, opens no device, port or socket, reads no secret and
  writes no file.
- `scada-conf` runs it as a child process under the user of `scada-conf`,
  not the driver's: that user cannot attach (the tag database checks the
  uid, tag database contract 11.2), is in no device group, and reads only
  the tree. The check mode opens no port, socket, GPIO or IIO file and reads
  no secret; its system calls are traced in test 22. How the limits are
  applied to the child, all without privilege: the executable is named by
  its absolute path in the install tree; the expanded file is passed as an
  open read-only descriptor (3), not as a path, and standard input is
  `/dev/null`; `PR_SET_NO_NEW_PRIVS` is set, so file capabilities of an
  executable are not gained; `PR_SET_PDEATHSIG` ends it with `scada-conf`;
  `RLIMIT_FSIZE` is 0 (it can write no file), `RLIMIT_NOFILE` 16,
  `RLIMIT_DATA` 256 MiB and `RLIMIT_CPU` 10 s; it runs in a process group
  of its own, which `scada-conf` kills with `SIGKILL` after 30 s of
  wall-clock time, so that a loaded unit does not refuse a valid tree; the
  environment is empty. Standard error is read up to 64 KiB into the log of
  `scada-conf` and never parsed. The same user owns the version store, so
  these limits, not the user, are what keep a faulty `configure` from
  writing it; test 22 checks each of them.
- Its standard output is outside input to `scada-conf`: at most 1 MiB and
  10,000 lines of at most 4 KiB, read by a strict line parser that is a fuzz
  target (section 14). An output over a limit, a malformed line, a code
  outside the protocol's range, or an exit status that disagrees with the
  lines (exit 0 with an error line, exit 1 with none) is CF-1012.
- **The files it checks.** Stage 7 writes the expanded files, with the
  `config_hash` of stage 6, to a private directory of `scada-conf`
  (`/var/lib/scada/conf/check/`, removed after the check) and runs each
  check there; commit step 1 writes the same bytes into the version. The
  check therefore reads exactly the bytes the driver will read.
- The address-space and time limits are build settings (#102): 256 MiB and
  10 s of CPU in a release build, larger in the sanitizer builds.
  A crash, a time-out or exit 2 is CF-1012 for that protocol: the tree is
  not committed, and the other protocols are still checked.
- **After a driver update.** `meta.json` records, per protocol, the build
  of the driver executable that checked the version. At its start
  `scada-conf` compares it with the installed executables; for a protocol
  whose driver changed it runs the check again on the active version, as
  part of the update's health check (section 11). If that check fails, the
  health check fails and the update is rolled back (reference 14.5), so the
  old driver keeps running the tree it checked. The claim above therefore
  reads: a tree is never refused by the build of the driver that checked
  it.
- What only a protocol knows (a native select and its window, an address
  the protocol cannot write, a block plan that cannot be made) is therefore
  refused before the commit, with the file and line of the configuration
  and a code of CF-650 to CF-699 (listed in the protocol's annex).
- Because the check reads the bytes the driver will read, a tree that
  `scada-conf` accepts is never refused by a driver on account of its
  configuration (the round trip is tested, 13.3).
- Offline validation (`--offline`, section 12.2) runs stage 7 only where the
  driver executables are installed; elsewhere it reports CW-1013 ("stage 7
  not run for protocol P") and the result cannot be committed. A commit on
  the unit always runs stage 7. Offline, the uids are not looked up, so the
  expanded files carry `config_hash: null`; the check mode does not read
  the hash.

### 13.2 Versions and the files beside a tree

```
/var/lib/scada/conf/
  versions/<n>/                     one complete committed version; <n> counts up from 1
    tree/                           the configuration files of section 1 (config_id covers these)
    meta.json                       version id, parent, author, time, comment, config_id, config_hash, format
    tag-set.json                    the tag set of section 12
    tag-set-names.json              the map of generated names (section 12.3), with config_hash
    expanded/<process>.json         one per process that loads configuration (13.3)
    signature                       phase 10 (reference 13.5)
  active -> versions/<n>            symbolic link to the active version
  apply.json                        the state of the last apply (13.6)
  staging/                          the staged tree being edited (reference 12.4)
```

- **Where the active tree lives.** The root file system is read-only
  (reference 4.4, 14.5) and everything that changes at run time lives under
  `/var/lib/scada`. The versions and the active link are therefore there.
  `/etc/scada` is installed by the package as a symbolic link to
  `/var/lib/scada/conf/active/tree`, so that reference 4.4 ("active
  configuration files") still names the active tree. Open: needs the
  product owner's ruling (O-5).
- **Version store.** Reference 12.4 says the store "should be a Git
  repository". This draft uses plain version directories instead. Git as a
  program is GPL-2.0, which `CLAUDE.md` does not allow until the licensing
  decision, and as a library it would be a new dependency whose writes do
  not sync the way reference 14.5 needs. History, diff and rollback are
  done by `scada-conf` over the version directories. A version is never
  changed after it is written; a rollback is a new version whose tree
  equals an older one. Open: needs the product owner's ruling (O-11).
- The files beside `tree/` are written by `scada-conf` only. They are not
  configuration: not validated as such, not in `config_id`, never edited by
  hand.
- **Retention.** Versions are never changed, but old ones are removed:
  `scada-conf` keeps the newest `conf.keep_versions` versions (section 3,
  default 50) and always the active one and the newest one whose
  `config_hash` is that of the `current` generation (13.5 needs it); it
  removes the others after a commit, oldest first.
- `staging/` is written file by file with the rule of commit step 1 (write,
  `fsync`, close, then the directory), so a power cut loses at most the
  file being saved; a staged edit is not a version until it is committed.
- **A changed uid.** A reinstall can change a uid without a commit. At its
  start, `scada-conf` builds the tag set of the active tree again; if its
  `config_hash` differs from the stored one, it commits a new version whose
  tree equals the active one (author `scada-conf`, comment "service uid
  changed", the old and new uids logged), and applies it (13.5). The old
  version is not changed.

Ownership and modes: everything under `/var/lib/scada/conf/` belongs to
`scada-conf`, group `scada`; directories `2750`, files `0640`. Nothing is
readable by other users: the tree holds the network layout of the site.
Every service is in group `scada` and reads; only `scada-conf` writes.

### 13.3 The expanded file

`expanded/<process>.json` is the interface between `scada-conf` (#102) and
the drivers and `scada-io` (#104, #107). It is written and read by one
library type of `libscada-config`, `ExpandedConfig`, and by nothing else.

- **Absent file.** `scada-conf` writes no expanded file for a process that
  is not in the tag set (a protocol with no channel). The absence of
  `expanded/<process>.json` in the version is what "no channel" means to
  the driver (13.7).
- **Content.** `format: "scada-expanded"`, `format_version: 1`, the
  `writer` (the `scada-conf` build that wrote it), `process`, `config_id`,
  `config_hash`, the names of the secrets the process may read, and its
  expanded channels, devices, points and outputs, each node with the
  configuration's file index and line (a table of the tree's file names
  heads the file), so that a driver reports a diagnostic against the file
  the user wrote (driver interface contract 4).
- **Values keep their written form.** Every scalar is stored as its source
  text with its resolved type (section 1.2): `10` and `10.0`, `0x7FFF`, a
  64-bit pattern and a decimal are kept as written, so that the loader
  types them exactly as stage 2 did. Nothing is stored as a JSON number. A
  value computed by a configuration expression has no source text: it is
  stored in a canonical text (an integer in decimal; a real as the shortest
  decimal that reads back to the same binary64; a string as it is) with
  its type and the location of the expression.
- **Limits.** An expanded file has its own limits, checked by the loader
  before the work they bound: 64 MiB, 8,000,000 nodes, nesting depth 32,
  and the scalar and key limits of section 1.3. They are sized for
  `limits.max_tags` (50,000 by default) in one process at about 40 nodes
  per point: 2,000,000 nodes; the #108 composition gives about 30,000
  points. `scada-conf` checks the same limits on the file it writes, so a
  tree whose expanded file for one process would pass them is refused at
  validation (CF-1016, stage 7), never by the driver's loader. A file that
  breaks a limit or fails to parse is refused as a whole; the driver runs
  none of it (13.5).
- **JSON subset.** UTF-8 without a byte order mark; objects, arrays and
  strings only (no numbers, `true`, `false` or `null`, except
  `config_hash: null` offline); no duplicate key in an object; the escapes
  of RFC 8259 and no others. The loader refuses anything else.
- **Format version.** The loader reads its own `format_version` and the
  one before it. An update that changes the version makes `scada-conf`, at
  its start, write a new version of the active tree with the new files
  (author `scada-conf`, comment "expanded format changed"), as for a
  changed uid (13.2); until then drivers read the old files.
- **Load time.** A driver loads and compiles the file on a thread of its
  own, never on the thread that writes the heartbeat (driver interface
  contract, process start), so that a large file delays the report, not the
  heartbeat; the 2 s of reference 15.2 is a target for the #108 composition,
  and a larger file is reported `late`, not lost.
- **Round trip.** Conformance test 21: for every tree of the corpus, the
  compiled configuration of each process from the tree in memory equals,
  field for field, the one compiled from the file written and read back. The
  loader is a fuzz target (section 14).

### 13.4 Commit

1. `scada-conf` validates the staged tree (stages 1 to 7, with stage 7 as in
   13.1) and writes the new version into `versions/<n>.tmp/`: each file is
   written, `fsync`ed and closed, then each directory is `fsync`ed, deepest
   first.
2. It renames `versions/<n>.tmp` to `versions/<n>` and `fsync`s `versions/`.
3. It writes `apply.json` (version `n`, the previous version, the kind of
   apply, state `started`, the start time) as a temporary file, `fsync`s
   it and renames it into place, so that it is never torn; then it creates a
   symbolic link `active.tmp` to `versions/<n>`, renames it over `active`
   (an atomic switch) and `fsync`s `/var/lib/scada/conf/`. From the rename
   on, the new version is the active one, also after a power cut.

`apply.json` alone does not say that a version became active; the link
does. At its start `scada-conf` compares the two: if `apply.json` says
`started` for a version that `active` does not name, the power cut came
inside step 3 before the rename; the apply is recorded as `not applied`,
and the version directory, never active, is removed like any newer
directory that `active` does not reference.

The kind of apply is decided at commit against the generation that is
running, and told to the user before they confirm (reference 12.4 step 5):

- **reload**: the new tag set's `config_hash` equals the `config_hash` in
  the header of the current generation (tag database contract 5);
- **restart**: it differs, and the data plane changes generation (reference
  5.5). Comparing with the running generation, not with the stored hash of
  the active version, also covers an active version whose generation was
  never built (a hung `scada-tagd`, below).

Only one apply runs at a time: a commit is refused (`APPLY_IN_PROGRESS`,
not a CF code: it is not about the tree) while `apply.json` says `started`.

### 13.5 The apply, step by step

Every service, `scada-tagd` included, watches `/var/lib/scada/conf/` for the
rename of `active` (an inotify watch on the directory) and also reads the
link every 5 s, so that a missed event delays an apply but cannot lose it.
Each service remembers the version number the link named when it last
handled a change, and acts only when the link names another one, so the 5 s
look does not repeat an apply.
`scada-conf` runs as its own user and signals no process: the link is the
only trigger, and no privileged helper takes part (open: needs the product
owner's ruling (O-13); reference 5.5 speaks of a reload signal). A service
opens the version directory the link names once and reads `meta.json` and
its expanded file from it, so that a second switch between two reads cannot
mix versions. `scada-tagd` is started with the tag set path
`/var/lib/scada/conf/active/tag-set.json` (its unit passes it, #105), not
the `/etc/scada/tag-set.json` of D-26.

**Reload** (same `config_hash`):

| Step | Who | What |
| --- | --- | --- |
| R1 | Every service in the tag set | Sees the change, reads `active/meta.json` and its own `active/expanded/<process>.json`, checks that both carry the same `config_id` and that the file's `config_hash` equals the generation's, writes `config_id` (the new one) and `config_state` 0 |
| R2 | The same | Applies what it can live (driver interface contract, reload), writes `sys.proc.<process>.config_id` and `config_state` 1, or `config_state` 2 with a log record if it cannot |
| R3 | `scada-conf` | Observes the tags (tag database contract 11.4; it does not attach) until every process in the tag set reports state 1 or 2 together with the new `config_id`, or 30 s have passed; then reports (13.6). A `config_state` read next to another `config_id` is a report of an earlier apply and is ignored, so a stale state 2 is never taken for this one |

**Restart** (another `config_hash`):

| Step | Who | What |
| --- | --- | --- |
| G1 | Every service but `scada-tagd` | Sees the change, finds that `active/meta.json` carries another `config_hash` than its generation, writes `config_state` 3 (waiting for the generation change; `config_id` stays the old one) and **keeps running unchanged** on the old generation with the configuration it has: polling, commands and outputs go on. It reads no new expanded file yet |
| G2 | `scada-tagd` | Sees the change, reads `active/tag-set.json`, checks that its `config_hash` equals the one in `meta.json` and that it builds (tag database contract 14), and runs the generation change of tag database contract 11.3: it builds the new generation, switches `current`, and sets the old header RETIRING. If the tag set does not build, `scada-tagd` keeps the old generation, logs why, and writes `sys.proc.tagd.config_state` 2 |
| G3 | Every service in the old generation | Sees RETIRING at its next heartbeat and applies `on_generation_change` (section 7.4) to its outputs. Drains and detaches as tag database contract 11.3 step 2 and the driver interface contract (generation change) say |
| G4 | Every service in the new tag set | Attaches to `current`, loads the expanded file of that generation (below), checks its `config_hash` against the new header, resolves, writes `config_id` and `config_state` 1 and resumes. A service that is not in the new tag set (a protocol whose last channel was removed) detaches and idles (13.7) |
| G5 | `scada-conf` | Observes the new generation until every process of it reports state 1 or 2 together with the new `config_id`, or 30 s have passed; then reports (13.6). Whether the apply took longer than `data_plane.generation_change_limit` (default 5 s, reference 5.5) decides `applied` or `late` |
| G6 | Every output writer | For an output that exists in both versions: if the writer is not running in the new generation when the limit has passed since it saw RETIRING, its outputs with `hold` go to their safe state (reference 5.5, section 7.4); the writer measures this itself, because `scada-conf` writes no output. Outputs that the commit removes are handled at G3 (below) |

- **The expanded file of a generation.** A service loads the expanded file
  whose `config_hash` equals the header of the generation it attaches to: the
  one in `active` if it matches, else that of the newest version not newer
  than `active` whose `meta.json` carries that `config_hash`. So a service
  that starts or restarts between G1 and G2 (the old generation is still
  `current`) runs the old configuration on the old generation, and moves at
  G3 with the others. A mismatch never stops polling while a matching
  version exists.
- **No matching version.** Only if no version carries the generation's
  `config_hash` (a version store damaged by hand) does the service run none
  of its channels, write `config_state` 2, log why, and wait for the next
  change of `active` or `current`.
- **G2 refused.** `scada-conf` sees `sys.proc.tagd.config_state` 2,
  switches `active` back to the previous
  version (the refused one never ran; this switch is not a new version),
  records it in `apply.json` and the history, and reports the apply failed.
  The services saw no RETIRING and never left the old configuration. Each
  sees the switch back as a change of `active` to the version it runs (the
  same `config_hash` as its generation and the same `config_id` it runs):
  it writes `config_state` 1 again and does nothing else; `scada-tagd`
  writes its own state back to 1. A tag
  set that `scada-conf` validated fails to build only on a fault of the
  unit (shared memory too small, tag database contract 3.4), so this is
  rare and is reported as a fault.
- **Removed outputs.** An output whose channel, device or own entry the
  commit removes has no writer in the new generation. At G3 its writer
  applies the output's old `on_shutdown` to it, as at an orderly stop of
  the writer, and then never writes it again; with `hold` the device keeps
  the output as it is. G6 does not apply to it.
- **A slow writer.** A writer that needs longer than the limit to drain,
  detach and attach (a long write deadline, 9.2 of the driver interface
  contract) sends every `hold` output it writes to its safe state on every
  tag-set change. This is the consequence of reference 5.5 and is put to
  the product owner with G6 (O-18).
- **`scada-tagd` hangs** (no new generation, no state 2, so no RETIRING is
  ever seen): nothing goes safe, because control is not lost: every service
  keeps running on the old generation with the old configuration, and
  stays in `config_state` 3. After 30 s `scada-conf` reports `incomplete`
  and does not switch `active` back: an accepted commit then stays
  unapplied, and the old configuration in force, until `scada-tagd`
  restarts (open: needs the product owner's ruling (O-19)).
  The apply completes when `scada-tagd` recovers: a restart by its watchdog
  computes the tag set of `active` (tag database contract 11.1) and runs G2.
- **G6 is fail-safe behaviour** and needs review by a person: an output
  writer that saw RETIRING and is not running in the new generation within
  the limit sends its `hold` outputs to their safe state, whether or not
  `scada-tagd`, `scada-conf` or the others are still working (open: needs
  the product owner's ruling (O-18)).
- A process that `scada-tagd` reports as not leaving the old generation
  (tag database contract 11.3 step 3, 30 s) is named in the report.

**Power cut or crash at each point.**

| When | What the next start finds | What happens |
| --- | --- | --- |
| Before the rename of commit step 3 (also after `apply.json` was written) | `active` on the old version; a `.tmp` or unreferenced newer directory; perhaps `apply.json` `started` for that version | `scada-conf` removes the directory and records the apply as `not applied` |
| After the rename of commit step 3, any later step | `active` on the new version; `/dev/shm` empty after a boot | `scada-tagd` builds the first generation from `active` (tag database contract 11.1); every service attaches with the new expanded file. `scada-conf` finds `apply.json` `started`, waits for the reports as in R3 or G5, and records the result with "after restart" |
| `scada-tagd` restarts between G1 and G2 | `current` on the old generation, another `config_hash` in `active` | At its start it computes the tag set of `active` (11.1 step 1), cannot reuse `current`, and builds the new generation as in G2 |
| A driver restarts between G1 and G2 | `current` on the old generation | It loads the old version's expanded file (above) and moves at G3 |
| A driver restarts after G2 | `current` on the new generation | It attaches there with the new file; G4 |
| `scada-conf` crashes during R3 or G5 | `apply.json` `started` | At its start it waits again for the reports, from the time it starts, and records the result |
| A power cut after `scada-tagd` refused G2 and before the switch back | `active` on a version whose tag set does not build; no generation after the boot | `scada-tagd` reports state 2 at its start; `scada-conf` sees `apply.json` `started` and the refusal, switches `active` back to the previous version and records `failed`; `scada-tagd` then builds from it |
| First start of a unit | No version and no `active` | The package installs a minimal tree (`system.yaml` only, #105); `scada-conf` commits it as version 1 at its first start; until then `scada-tagd` and the services wait for `active` |

### 13.6 What the apply reports

The result is written to `apply.json` and the version history, logged, and
returned to the caller of the commit (CLI or API) if it still waits:

| Result | When |
| --- | --- |
| `applied` | Every process of the (new) tag set reported the new `config_id` with state 1 within the limit (2 s reload, 5 s restart) |
| `late` | The same, after the limit; the time taken is given |
| `failed` | A process reported state 2 (its log record is attached), or G2 was refused (the link went back, 13.5); the operator decides whether to roll back by a new commit |
| `not applied` | A power cut inside commit step 3 before the rename (13.4): the version never became active |
| `incomplete` | A process did not report within 30 s (also when `scada-tagd` built no generation, 13.5); it is named, with its `config_state` if it has one |

The apply's history entry keeps the per-process times, so that the 2 s of
reference 15.2 and the 5 s of reference 5.5 are measured on every apply
(#108 reads them).

### 13.7 Driver processes with no channel

- Every driver service and `scada-io` is installed and enabled by its
  package and is always running, also when its protocol has no channel in
  the active version (it is then not in the tag set, section 12.2).
- Such a process finds no expanded file for itself in the active version
  (13.3), tells the service manager it is ready (its unit is of type
  `notify`, #105), keeps sending the watchdog notification as a running
  driver does (the main thread notifies whether attached or not, also
  between a detach and the next attach), logs once that it has nothing to
  run, attaches to nothing, and watches `active` as in 13.5. While idle it
  has no heartbeat and no tag, since it is not in the tag set; only the
  service manager shows it, which is correct. When a commit gives its protocol
  a channel, the new tag set holds its process; it attaches at G4 once
  `current` is a generation whose process table holds it, looking again at
  each change of `current` (it watches the base directory of the tag
  database too).
- When the last channel of a protocol is removed, its process is not in the
  new tag set: at G4 it detaches and idles.
- An idle driver costs one process with no worker and no connection. It
  never exits on its own, so the service manager never counts it against
  the restart limit of reference 14.4, and no privileged helper (`scada-sys`)
  is needed in phase 2 to start or stop drivers.
## 14. Conformance tests

The configuration schemas are accepted against a corpus of trees in
`libs/scada-conf/tests/corpus/` (P2-03 to P2-07). Each case is a tree and an
expected result: for a valid tree, the expanded tag list, the expanded files
of section 13.2 and the tag set with its `config_hash` for a fixed uid table;
for an invalid tree, the list of `(file, line, code)` it must give, in the
order of section 1.4. The tests run on x86-64 and ARM64.

1. Every example of this contract and of reference 12.3, assembled into one
   tree, is valid, and gives a pinned tag set and `config_hash`.
2. One tree per rule of section 10, each giving exactly its code at the
   expected line; a warning-only tree commits.
3. YAML subset and typing: an anchor, an alias, a merge key, a tag, two
   documents, a duplicate key, a duplicate after typing (`16` and `0x10`),
   `null`, `yes`, `True`, `010`, `+5`, `.5`, `1_000`, a byte order mark, a
   NUL, each refused with its code and the right column; `label: 0123` read
   as a string; `gain: 10` and `gain: 10.0` giving `int64` and `float64`.
4. Limits: one case per limit of section 1.3, each refused before the work it
   bounds (a range `"1..9223372036854775807"` is refused without iterating;
   a tree that passes `limits.max_tags` stops expanding); a symbolic link, a
   FIFO and a path outside the grammar in the tree; 1,001 errors give 1,000
   and a count.
5. Error order: a tree with errors in several stages and files gives only
   the first stage's errors, sorted by section 1.4; a syntax error in one
   file still gives the schema errors of the others.
6. Expansion: a corpus of templates and instances (base and protocol
   template, `extends` to depth 4, parameters of every type, `when`, template
   and instance `disable`, `only`, `repeat` with one and two variables,
   irregular and empty ranges, overrides of maps and lists, exclusive
   groups, `replace: true`, an override that uses a `repeat` variable,
   appended `identify`, instance points) with the expected expanded tag sets;
   each step of the merge order of section 4.5 has a case where a different
   order would give a different result (#103).
7. An error inside an expanded instance names the instance and the template
   file and line (#103).
8. Name length: a prefix and point name of exactly 127 bytes is valid; 128 is
   CF-402 at the instance, naming the template line; the same for a generated
   tag (CF-403); a device name of 101 bytes is CF-212 (#119).
9. `heartbeat_check_ms` 400 with a `retain_now` tag is valid; 401 is CF-1010
   with its message (#96).
10. `retain_max_age` on an internal tag is valid with CW-510; on an output
    mode tag (through an override), an authority tag, an alarm state tag and
    a counter it is CF-508 (#89).
11. Expressions: every operator and function of section 5.6 with every type
    it accepts and one it refuses; `250 / 1000` is 0.25; overflow and
    division by zero giving BAD with OVERFLOW; `worst` and `ignore_bad`;
    patterns, cycles, ambiguous attributes, reserved keywords. Syntax cases
    that must parse: `n IN 1..5`, `n IN 1 .. 2 * k`, `x.quality = GOOD`,
    `x.age > T#5s`, `flow.rate > 3.5`, `level.min`, `power.avg + power.max`,
    `energy.sum`, `in.flow`, `mod.x`, a tag named `sum`, `0x10 + n`,
    `16#FF`, `2 ** -3`, `-x ** 2` (as `-(x ** 2)`), `2 ** 3 ** 2` (as
    `2 ** (3 ** 2)`), `NOT level < 5` (as `(NOT level) < 5`, then CF-523 on
    an INT tag); and `for` values `1..inputs`, `0 .. n - 1`,
    `16#10..16#1F`; `a - -b` and `a * -b` (one unary operator after a
    binary one is allowed); `NOT x IN s` as `(NOT x) IN s` and `NOT a = b` as
    `(NOT a) = b`, both then CF-523; `ps1.pump-speed` as `ps1.pump - speed`,
    reported as an unknown name with a hint to `tag("…")`; `t#5s`,
    `Sum("ps1.*")`. Cases that are CF-520: `3x`, `5mod 3`, `x = 1or y`,
    `10ms`, `T#5S`, `0x1G`, `a < b < c`, `NOT NOT a`, `- -x`,
    `1 + 2..5`, a bare `1..5`, `x = 1..3`, `in`, `x IN`, `n IN 1..`,
    `1.`, `.5`, `x.`, `a == b`, `1..2..3` as a `for` value. CF-528:
    `9223372036854775808`, `-9223372036854775808`, `1e-400`. CF-529:
    `foo(1)`. `avg` of 3 and 4 is 3.5. The results
    are bit-identical on both architectures, except for the transcendental
    functions and `**`, which may differ by one unit in the last place.
12. Scaling and decoding vectors, shared with the driver framework's tests
    (driver interface contract): raw bytes plus a point give the value's
    bits, quality and flags, bit-identical on both architectures, for every
    raw type of Annex A.2 in every byte and word order, `field` with sign
    extension, `mod10k`, `epoch`, `datetime` in a repeated and a skipped
    local hour, exponents of −20 and 20, every `on_invalid`, a marker with
    `word_order: little`, `raw_range`, a double point with each shape and a
    `states` map, an enumeration with an unknown code, a counter's wrap,
    saturation and decrease, and the inverse scaling of section 7.2 (12.5 kW
    on 0.1 kW is raw 125; a NaN request is refused; a `{ raw: 0x7FFFFFFF }`
    release is written as it stands; on an INT64 target with no `scale`,
    2^53 + 1 = 9007199254740993 is written as 9007199254740993, and with
    `gain: 3` a value of 9007199254740995 gives raw 3002399751580331, while
    9007199254740996 is refused as not divisible; 2^63 − 1 on INT64 and
    2^64 − 1 on UINT64 with no `scale` are written exactly; the driver
    framework's tests use the same vectors).
13. Tag set: the same tree in a different order of files and entries, and
    with files renamed (where names allow), gives the same `config_hash`; a
    tree that changes only an alarm limit, a poll period, a scale or a
    logging setting gives the same `config_hash`; a tree that adds one tag
    gives another (#101, tag database conformance test 20). Every tag set
    the corpus builds passes `scada::tag::validate`; a tree beyond a limit of
    tag database contract 14 is refused with its file and line. A channel of
    100 tags gets a ring of 1,024 entries, one of 1,000 tags 4,096 (section
    12.3).
14. Short names: the corpus pins `C()` and `T()` of a short and a long name;
    adding, removing, reordering and renaming one channel or task leaves
    every other owner and queue name unchanged; two long names given equal
    short names by a test hook are CF-1006; the name map lists every
    generated name.
15. `config_id` is the same for one tree on two uid tables, where
    `config_hash` differs; it changes with any byte of any file, also when
    only the line ends change from LF to CRLF.
16. Apply (#102), against the step tables of section 13.5: a power cut
    simulated before and after each commit step leaves the old or the new
    version active, never a mixed one, and the next start removes the
    partial directory; a reload reports `applied` within 2 s; a restart
    commit keeps every driver polling until RETIRING (a simulator counts
    requests without a gap longer than one period before G3); a driver
    restarted between G1 and G2 runs the old version's expanded file and
    moves at G3; `scada-tagd` restarted between G1 and G2 builds the new
    generation; a tag set that fails to build at G2 sends `active` back and
    reports `failed`, with no RETIRING seen; a driver started with no
    channel reports ready, stays running, and attaches when a commit gives
    its protocol a channel; a second commit during an apply is refused.
17. Protocol validation (section 13.1): one tree per CF-65x code of Annexes A
    and B, refused at validation with the configuration's file and line.
18. A tree of format 0 (a test format) is migrated to format 1 by a test
    migration, keeping the comments of lines it does not change.
19. Modbus (Annex A): addresses of every type and order, overlaps between
    devices with shared unit ids (refused and allowed cases), point-level
    unit ids, functions outside the supported set, side-effect ranges shared
    by several points and refused across poll groups, `device` paths and
    duplicates, literal addresses only; `lost_after_timeouts` outside 1 to
    10 refused.
20. Local I/O (Annex B): lines and channels named by label, refused by
    number; outputs without `at_start`; an explicit `every_cycle` gives
    CW-660; a line, chip or IIO channel that is not present gives no
    validation code (the check mode opens nothing, test 22); the stop step's set (outputs with `on_shutdown` `safe` or
    `release`) read from an expanded file. The behaviour (control loss,
    shutdown, the stop step after a crash, `at_start`) is tested by the
    driver interface contract's tests with the simulator backend reporting
    line levels.
21. Expanded file (section 13.3): for every tree of the corpus, the compiled
    configuration of every process from the tree in memory equals the one
    compiled from its expanded file written and read back; `10` and `10.0`,
    a 64-bit pattern and a decimal keep their type and value; a file over a
    limit, of another `format_version`, or cut short is refused as a whole.
22. Check mode (section 13.1): a protocol's `--check` on a tree of test 17
    gives the same codes and locations as the corpus; a check that crashes,
    exceeds 10 s or exits 2 is CF-1012 and the tree is not committed; the
    system calls of the check mode (traced) open no socket and no device.

**Fuzz targets** (`CLAUDE.md`: every parser of outside input): the YAML
subset parser, the expression parser, the structured-text parser and the
template expander (a fuzzed tree in, an expanded tree or located errors
out, within the limits of section 1.3) and the loader of the expanded file
(section 13.3) and the parser of the check mode's output (section 13.1),
each with a seed corpus taken from
the conformance corpus (#100).

**The 300-device test (#108)**, proposed composition, so that it covers
what the reviews found at scale:

- 200 Modbus TCP inverters on 10 gateway channels of 20 units each, at
  100 ms device pacing (CW-606 shows for one of them), 20 devices of one
  gateway dead (the many-dead-devices case) and one dead unit behind
  another gateway, whose 19 live units must stay GOOD with no reconnect;
- 80 Modbus TCP devices with a connection each (80 owners: the ring and
  queue figures of section 12.3);
- 20 Modbus RTU devices on 2 serial lines at 19,200 bit/s, through
  pseudo-terminals (which do not test line timing; that is the board test
  of #41);
- pass: the achieved `period.<group>` of every live device within 10 % of
  its configured period, with the database size and process memory
  reported. This is the one composition; the driver interface contract
  cites it.

## 15. Open rulings

Each place marked "open" above is drafted with the option this contract
proposes. The product owner rules on each; until then the contract stays a
draft. The committee of the lead session reviewed versions 0.1 and 0.2
(C++, SCADA, Linux); its recommendation is given beside each. Each ruling
has one id across this contract and the driver interface contract: a ruling
listed in both is the same ruling, decided once.

| Ruling | Subject | Options | Drafted as | Committee |
| --- | --- | --- | --- | --- |
| X-1 | A select on the device for protocols where reference 6.4 assumes none | (a) keep 6.4; (b) native select per device and point | (b), declared by the protocol; Modbus coil roles reserved | (b); safety review |
| X-2 | Channels opened by the device | (a) outbound only; (b) listening, still master; (c) server role | `role`, only `connect` | Keep direction open; no server role |
| X-3 | One device on a main and a backup connection | (a) connection group; (b) device under two channels; (c) none | (c) now; `connections` reserved for (a) | As drafted |
| X-4 | Several devices on one device address, one device on several | (a) relax; (b) one device; (c) point unit id; (d) sub-devices | (a) with disjoint ranges, plus (c) | As drafted; failure count per (channel, unit id) in the driver |
| X-5 | Refreshing a setpoint without a new operator action | (a) refreshes under the request's record; (b) a record per refresh; (c) none | (a), `setpoint` only, with the `refresh` tag and an alarm when it ends unplanned | (a), stated explicitly. Dissent of the SCADA reviewer: a new request that the driver refuses should not end the refresh of the old, valid setpoint; as drafted it does |
| X-7 | Device select windows shorter than the gateway's select | (a) gateway never longer, validated; (b) select and operate at OPERATE | Per output: `at_select` (validated) or `at_operate` | As drafted; safety review |
| X-8 | Several protocols of one device type | (a) bindings per protocol; (b) one template per protocol on a base; (c) independent | (b) now, `bindings:` reserved for (a) | As drafted |
| X-9 | Release or safe value written to a remote setpoint | (a) stop refreshing only; (b) also write one | (b): `on_shutdown`, `on_control_loss` with `hold`, `safe`, `release` | As drafted; fail-safe, person review |
| X-12 | Who sets device clocks | (a) none in phase 2; (b) a service, default off | (a) | As drafted |
| X-13 | Unservable pacing: error or warning | (a) a warning (CW-606, CW-608); (b) an error | (a) | The reviewers differ: one asks for an error when the estimated cycle of a poll group exceeds the smallest `max_age` of the group, and a warning otherwise |
| X-14 | Feedback with four states | Accepted or not | Double-point feedback accepted | As drafted |
| X-15 | Point lists from device files | (a) accept generated files; (b) an importer now | (a) | As drafted |
| X-16 | Settings edits and setting-group switching | (a) both out; (b) group switching as a command | (b) as an `on_only` output; settings edits out | As drafted |
| X-17 | Modbus function codes | All required, or a minimum set with a per-device policy | Minimum set, `functions` | As drafted |
| O-1 | Fail-safe writes and "no audit, no action" (section 7.4) | (a) a fail-safe write waits for its event record; (b) it is made, then journalled best-effort | (b) | (b): reference 6.5 governs; the audit service may be down in the same failure |
| O-2 | AUTO writes while `scada-cmd` is down (driver interface contract) | (a) refused (the mode tag reads OWNER_DOWN); (b) the stored mode is used when OWNER_DOWN is its only defect | Driver interface contract | (b) from the SCADA reviewer: the mode cannot change while `scada-cmd` is down. Not reviewed by the C++ reviewer, who notes that (b) uses the value of a BAD tag, which tag database contract 6.2 forbids; if accepted, the exception must be written against 6.2 |
| O-3 | One stalled channel ends the whole driver process (driver interface contract) | (a) accept; (b) several processes per protocol (`process_group`, reserved here) | (a), `process_group` reserved | (a) for phase 2, stated as a consequence |
| O-4 | Real-time protocol drivers and scan groups in phase 2 | (a) only `scada-io` real-time; (b) Modbus real-time too | (a); `scan_group` reserved in both contracts | (a) |
| O-5 | Where the active tree lives (section 13.2) | (a) versions under `/var/lib/scada`, `/etc/scada` a link to the active tree; (b) files in `/etc/scada` (reference 4.4) | (a) | (a): read-only root (reference 4.4, 14.5) |
| O-6 | `config_hash` depends on the unit's uids; `config_id` for comparisons across units (sections 12.2, 12.8) | (a) as drafted; (b) user names in the tag set (a change of tag database contract 1.0) | (a) | (a) |
| O-7 | Host names for TCP channels (driver interface contract, TCP connections). NTP servers are not covered: the time client resolves its own names | (a) literal addresses only in format 1, names reserved; (b) names, resolved outside the workers | (a) | (a) |
| O-8 | Generated short names for owners and queues (section 12.3) | (a) readable prefix plus hash where needed; (b) shorter user names | (a) | (a) |
| O-9 | Session steps with secrets (a login at connect) | (a) out of phase 2, `session` reserved; (b) in phase 2 | (a) | (a); needs the security review |
| O-10 | Heartbeat or watchdog register of a device (reference 14.4 "should") | (a) not in phase 2, a device key reserved; (b) in phase 2 | (a): no surveyed device documents one; `refresh` covers setpoints | (a) |
| O-11 | The version store (section 13.2) | (a) plain version directories handled by `scada-conf`; (b) a Git repository as reference 12.4 suggests (the program is GPL-2.0; a library with a linking exception is a new dependency whose writes need the sync of reference 14.5) | (a) | (a): no GPL program (`CLAUDE.md`), sync control (reference 14.5) |
| O-12 | Remote switching outputs hold by default on control loss and shutdown (section 7.2, CW-721) | (a) `hold` default as reference 14.4, with a warning; (b) `safe` default for switching outputs on protocol drivers | (a) | (a) with the warning (SCADA reviewer); the owner should see the consequence |
| O-13 | How a commit reaches the services (section 13.5) | (a) every service, `scada-tagd` included, watches the `active` link, no signal and no helper; (b) the reload signal of reference 5.5, sent through the privileged helper `scada-sys` | (a). Consequences: an apply starts up to 5 s late when an inotify event is lost; every service needs read access to `/var/lib/scada/conf/`; drivers with no channel run idle all the time; nothing in phase 2 starts or stops a service | (a): needs no privilege (Linux reviewer) |
| O-14 | Reserved words in expressions (section 5.6) | (a) only the operator words and literals, only as a one-segment name; function names only before `(`; (b) every keyword and function name reserved everywhere (0.2) | (a) | (a): (b) forbids `x.quality` and common point names (C++ reviewer) |
| O-15 | Write groups (a curve or table written in one request) | (a) none in this version; (b) in phase 2 | (a) | (a) |
| O-16 | Pulses timed by the unit (section 7.2, CF-720) | (a) not in this version: in the device, or a control program with a timer block; (b) a framework pulse with an off write | (a) | (a), with the guidance on where such a pulse belongs (SCADA reviewer) |
| O-17 | How `scada-conf` runs a protocol's validation (section 13.1) | (a) the driver executable's check mode as a child process; (b) `scada-conf` links every protocol library | (a) | (a): a protocol's licence and faults stay in its own process (D-81; Linux reviewer) |
| O-18 | A writer that is not running in the new generation within `data_plane.generation_change_limit` sends its `hold` outputs safe (G6, section 13.5) | (a) as drafted, each writer measuring the limit itself; (b) outputs hold whatever the duration, with an alarm only | (a), reference 5.5. Consequence: a writer slower than the limit (a long write deadline) trips every `hold` output it writes on every tag-set change; outputs that the commit removes get their old `on_shutdown` instead (13.5) | Person review (fail-safe) |
| O-19 | `scada-tagd` hangs during a tag-set change (section 13.5) | (a) nothing is made safe, the old configuration stays in force after the accepted commit, the apply is `incomplete` and completes when `scada-tagd` restarts; (b) `scada-conf` switches `active` back after the limit | (a): control is not lost, so nothing must go safe; switching back would race a `scada-tagd` that resumes | Lead: state it for the owner |
| O-20 | Nothing is promised after the stop step of `scada-io` releases a line (Annex B.4) | (a) a deployment requirement: a controller that keeps the level, or a hardware pull to the safe level; (b) a process that keeps holding the lines until `scada-io` starts again, with a hand-over | (a) | Linux reviewer: say so as a requirement |
| O-21 | Switching outputs without `interlocks` (section 7.2, CF-711; reference 12.5 does not ask for the key) | (a) the key is required, an empty list allowed with a warning (CW-712); (b) optional | (a) | As drafted |

X-6 (replayed events), X-10 (driver interface shape), X-11 (STALE for
reported points) and the R rulings of the driver interface contract belong
to it; this contract refers to their answer where it uses them: R-6
(SUBSTITUTED for values substituted by configuration, not for `keep_last`),
R-7 (scaling bit-identical, contraction off, D-98), R-11 (fail-safe writes
repeated until confirmed; recommendation "accept, with the condition
defined"), R-13 (no RS-485 direction control in phase 2: Modbus RTU needs
an adapter or board that switches direction itself), R-15 and R-17
(`lost_after_timeouts`, and what a driver-made close does to the other
devices, also when the reconnect is not within one deadline), R-16
(`local_blocks_auto` fails closed on a panel input that is not GOOD, which
also blocks automatic stops and freezes the area in its state) and R-18
(the definition of "control lost", which the fail-safe state machine of the
driver interface contract uses).

## 16. Differences from the reference document

None of these changes what the product is; each is listed so that a reader of
the reference finds it.

1. One channel per file in `drivers/<protocol>/`, its file name equal to the
   channel's name; templates without subdirectories (reference 12.1:
   `drivers/<driver>/*.yaml`, `templates/*.yaml`).
2. Any file not listed in section 1, also a reserved one or an editor's
   backup, is an error.
3. `enums.yaml` is a file of the tree.
4. `format` in `system.yaml`, optional while the tree is of format 1.
5. Command queues per driver process and source as reference 5.1, with
   generated short names for owners (O-8); event queues per control task
   (`events.control.<T>`) and per driver.
6. A disabled channel or device keeps its tags.
7. `config_id` beside `config_hash`; `config_hash` depends on the unit
   (O-6).
8. `interlocks` required on switching outputs (reference 12.5 does not ask
   for it); interlocks per direction.
9. New output keys: `native_select`, `refresh`, `on_control_loss` with
   `release`, `auto_deadband`; point keys `storage` (required),
   `write_limits`, `dangerous`.
10. A point-level `unit_id` and devices that share a unit id (reference
    12.5, "no overlapping or duplicate device addresses on a channel"; X-4).
11. `heartbeat_check_ms` at most 400 in every tree with an output or an area.
12. Adding or removing an alarm, an output or a tunable parameter restarts
    the data plane (implied by reference 5.4).
13. The active tree is under `/var/lib/scada`, `/etc/scada` a link to it,
    and versions are plain directories, not Git (reference 4.4, 12.4; O-5,
    O-11).
14. Fail-safe writes do not wait for an audit record (reference 5.3, 8.2;
    O-1).
15. Output status value 6, ACCEPTED_UNVERIFIED (reference 8.1 lists 0 to 5).
16. SUBSTITUTED is also set for a value substituted by configuration and
    for a value the device reports as forced (tag database contract 6:
    "entered by a person"; R-6 of the driver interface contract).
17. Only `control` and `io` are real-time in phase 2 (reference 4.3 includes
    "any driver assigned to a fast poll group"; O-4).
18. The expression grammar departs from IEC 61131-3 in `**` and repeated
    unary operators, and `/` is true division (section 5.6).
19. `realtime.cpus` is in `system.yaml` (reference 4.3 leaves the place
    open; the real-time threads of every process use it).
20. A commit reaches the services by a watched link, not by the reload
    signal of reference 5.5, and `scada-tagd` starts the generation change
    itself when it sees a new tag set (section 13.5; O-13).
21. Fail-safe writes are repeated until confirmed (reference 14.4 says
    nothing on repeats; section 7.4, R-11 of the driver interface contract).
22. `latch` is reserved: it behaves as `single` (reference 8.1 lists it).

## 17. Change log

| Version | Date | Change |
| --- | --- | --- |
| 0.1 | 2026-11-22 | First draft (issue #98, P2-01), from the outline in `README.md`, reference 1.1 and the device survey of 2026-11-22 (not in the repository). Decisions D-69 to D-80. |
| 0.2 | 2026-11-22 | Committee review of 0.1 (C++, SCADA, Linux) and the lead's directions: this contract owns every key (section 4.10, Annex C); scalar typing and limits (1.2, 1.3); validation stages and order (1.4); the expression grammar completed, `/` true division, logical operators BOOL only (5.6); merge rules for expressions, exclusive groups and `replace` (4.5); `only`, template `disable`, `bit_order` required, `storage` required, `write_limits`, `invert`, `restart_indication`, `device_time` with trust; `timeout` defined (4.11); side effects on register ranges (A.4); command kinds and inverse scaling (7.2); one set of fail-safe keys (7.4); queues per driver process, readable short names, owners per thread, rings sized per owner (12.3); `config_id` details; Apply (13); the `io` schema (Annex B); open rulings with the committee's recommendation (15); differences from the reference (16). Decisions D-69 to D-80 revised, D-100 to D-102 added (D-81 to D-99 are the driver interface contract's). Migration: none (draft). |
| 0.3 | 2026-11-23 | Delta check of 0.2 (C++, SCADA, Linux) and the lead's directions, agreed with the driver interface draft: one apply sequence, triggered by the watched link, with step tables, power cuts, the report and idle drivers that never exit (13.4 to 13.7, D-100); the driver's check mode instead of linked protocol libraries, and the expanded file's format (13.1, 13.3); a changed uid makes a new version (13.2); `lost_after_timeouts` split into write and read cases (4.6); `timeout` cites the driver interface's frame time (4.11); the grammar fixed (no range as an operand, reserved words only as one-segment names, `0x` literals, literal bounds) and checked mechanically (5.6, D-78); `avg` REAL; `keep_last` UNCERTAIN and STALE, then BAD; `states` integer keys; exact integer inverse scaling; `double` targets INT64; `safe_state` per kind; `latch` reserved; fail-safe writes repeated (`failsafe_retry`, `<output>.failsafe`); `local_panel` as an input tag; shared command registers; `only` in templates; joined `repeat` bindings; the stage of each code; `scan: auto`, the stop step and `at_start` in Annex B; NTP host names allowed; rulings O-5 split, O-11 to O-17 added, one id per ruling across both contracts. New codes CF-422, CF-528, CF-529, CF-640, CF-718 to CF-720, CW-721, CF-1012, CW-1013, CF-1014, CW-1015, CW-660, CW-661; CF-660 and CF-661 removed. Migration: none (draft). |
| 0.4 | 2026-11-23 | Third pass on 0.3 (C++, SCADA, Linux) and the lead's directions, agreed with the driver interface draft: the fail-safe state machine is the driver interface contract's (8.13) and 7.4 cites it; new keys `control_attach_grace`, `failsafe_max_refusals`, and `failsafe_retry` as the first interval of a back-off; owner and meaning of `<output>.failsafe`; removed outputs and slow writers at a generation change (13.5, O-18); the `scada-tagd` hang (O-19); stages 6 and 7 swapped so that the check reads the bytes the driver reads; `apply.json` against the link after a power cut; the kind of apply against the running generation; reports paired with their `config_id`, state 3, `incomplete` after 30 s; check mode enforcement, re-check after a driver update, expanded-file limits, format version, computed values, JSON subset, load thread; CW-661 removed (validation opens no hardware); idle drivers keep the watchdog; exact integer inverse scaling with no `scale`; CW-721 only when both keys are absent; CF-722, CF-723, CF-1016; lexical rules (abutting tokens, case); corpus cases; `lost_after_timeouts` quotes the driver interface rule word for word; `apply.json` by rename, the switch back after a refused G2, a power cut before it, the first start, retention (`conf.keep_versions`); check-mode enforcement without privilege; re-check in the update's health check; the stop step's version, form and bound; `auto` by ancestor bus; A.3 and A.5 on shared command registers; rulings O-18 to O-20, X-5 dissent, consequences of O-13, R-11, R-16, R-17. Migration: none (draft). |
## Annex A. The Modbus protocol schema

The protocol keys of `drivers/modbus/*.yaml` and of templates with
`protocol: modbus`. Section 4.1 says how they combine with the framework
keys. `scada-conf` runs the protocol's `configure` on them (section 13.1).

### A.1 Channel and device keys

| Key | Object | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| `type` | channel | `rtu`, `tcp`, `rtu_over_tcp` | required | `rtu_over_tcp`: RTU frames with their CRC over a TCP connection (serial servers, some meters' ports). `ascii` and `udp` are reserved |
| `device` | channel, `rtu` | absolute path under `/dev/`, no `..`, at most 255 bytes | required | The serial port. Prefer the stable names (`/dev/serial/by-id/...`, `/dev/serial/by-path/...`): a USB adapter's `ttyUSBn` changes when it is plugged in again. A link must stay within `/dev`. Two channels on one path are CF-620; the driver opens the port exclusively |
| `baud` | channel, `rtu` | 300 to 4,000,000 | 19,200 | A rate the port refuses is a connect error with its own diagnostic, never the nearest rate |
| `data_bits` | channel, `rtu` | 8 | 8 | RTU frames have 8 data bits; 7 belongs to ASCII, which is reserved (CF-622) |
| `parity` | channel, `rtu` | `none`, `even`, `odd` | `even` | |
| `stop_bits` | channel, `rtu` | 1, 2 | 1 | |
| `turnaround` | channel, `rtu` | duration, `0ms` to `1s` | `0ms` | Least silence before the next request, on top of the 3.5 characters of the protocol, measured from the last byte received |
| `rs485` | channel, `rtu` | reserved | | Direction control by the kernel (`direction`, `rts_active`, delays, `receive_own`). Phase 2 relies on adapters and transceivers that switch direction themselves; the driver never toggles modem lines |
| `host` | channel, `tcp`, `rtu_over_tcp` | IPv4 or IPv6 address, written literally | required | Host names are reserved (open: needs the product owner's ruling (O-7)): resolving a name has no deadline the driver can enforce. Loopback, multicast and broadcast addresses are CF-622 |
| `port` | channel, `tcp`, `rtu_over_tcp` | 1 to 65,535 | 502 | Devices use other ports too. Two channels to one host and port are CW-621 |
| `connect_timeout` | channel, `tcp`, `rtu_over_tcp` | duration, `100ms` to `60s` | `3s` | |
| `local_address`, `tls` | channel, `tcp`, `rtu_over_tcp` | reserved | | A fixed source address (for devices with client allow-lists); TLS with secret references |
| `connections` | channel | reserved | | Main and backup connections for one device (X-3) |
| `unit_id` | device | 0 to 255 on `tcp` and `rtu_over_tcp`; 1 to 247 on `rtu` | required | Unit 0 on TCP is an ordinary unit (some devices answer only on it); a gateway may answer for itself on 255. RTU broadcast (unit 0) is not used |
| `functions` | template, device | list from 1, 2, 3, 4, 5, 6, 15, 16 | all eight | Functions the device supports. The driver uses no other (CF-615), and substitutes where it can: 16 for 6, repeated 5 for 15, 3 for 4 only where `read.input_as_holding` says the tables are the same. Function 23 is reserved. Open: needs the product owner's ruling (X-17); this reads reference 6.4 as the minimum set |
| `address_base` | template, device | 0, 1 | 0 | `register` values are as the device documents them: with base 1, register 1 is protocol address 0. No other arithmetic is applied: a documented "40001" is written as the register the document means with its base, never as an offset of 40,000 |
| `read` | template, device | read rules (A.4) | as A.4 | |

### A.2 Addresses

```yaml
address: { table: holding, register: 200, type: float32, word_order: little }
address: { table: holding, register: 210, type: uint16, field: [4, 7] }
address: { table: coil, register: 12 }
address: { table: holding, parts: [ { register: 31 }, { register: 30 } ], type: uint32 }
address: { table: holding, register: 300, type: string, length: 16, encoding: ascii }
address: { table: input, register: 40, type: epoch, width: 32, resolution: 1s }
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `table` | `coil`, `discrete`, `input`, `holding` | required | The four Modbus tables. The read function follows from it: 1, 2, 4, 3 |
| `register` | integer or expression | required unless `parts`, or a write-only point with `write.on` and `write.off` | The address in the table, with `address_base`; 0 to 65,535 after the base (CF-603) |
| `type` | below | `bool` for coils and discrete inputs, `uint16` otherwise | Raw type |
| `byte_order`, `word_order` | `big`, `little` | `big`, `big` | Byte order within a register, register order within a multi-register value; template default, overridable per instance (`defaults`) and per point |
| `bit` | 0 to 15 | none | One bit of a register: a BOOL, numbered by the template's `bit_order` |
| `field` | `[lo, hi]` | none | A bit field of a register or of a multi-register value, extracted and shifted down. With a signed raw type (`int16`, `int32`), the field is sign-extended from bit `hi` |
| `byte` | `high`, `low` | none | For `int8`, `uint8`: which byte of the register |
| `parts` | list of `{ register }`, most significant first | none | A value from registers that are not adjacent, in this order |
| `length` | 1 to 256 bytes | required for `string`, `bytes` | |
| `encoding` | `ascii`, `utf8`, `latin1` | `ascii` | For `string`; `padding` (`nul`, `space`) is stripped |
| `count` | 2 to 4 | required for `mod10k` | Registers of a modulo-10,000 value: each register holds 0 to 9,999 (unsigned); `word_order: little` (the usual form) puts the least significant first; a register above 9,999 makes the value BAD with OUT_OF_RANGE |
| `width`, `resolution` | 32 or 64; `1s`, `1ms`, or `<n>ns` | required for `epoch` | Integer time since 1970-01-01 (other bases are reserved) in steps of `resolution`; converted to INT64 ns UTC, with `device_time` |
| `fields` | list of `year`, `year2`, `month`, `day`, `hour`, `minute`, `second`, `ms` | required for `datetime` | One field per register, from `register` on, in this order; `year2` is a two-digit year of 2000 to 2099. Converted with `device_time`. Packed and BCD forms are reserved |
| `device_time` | as for a device (4.6) | the device's | For one value whose time base differs from the device's |
| `unit_id` | as the device's | the device's | The unit id of this point, for one device that answers on several unit ids (open: needs the product owner's ruling (X-4)) |
| `write` | write encoding (A.5) | plain write of the value | For points with `access: rw` or `wo` |

Raw types: `bool`, `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`,
`int64`, `uint64`, `float32`, `float64`, `string`, `bytes` (as `string`
without decoding, shown as hex), `mod10k`, `epoch`, `datetime`. A 24-bit value
in a 32-bit pair is `uint32` (or `int32`) with `field: [0, 23]`. A value of
more than one register may start at any register, odd or even. A string tag
holds at most 250 bytes (tag database contract 14): a longer device string is
cut to 250 bytes and written with OVERFLOW.

Double points (`kind: double_point`, section 5.3) take one of these address
shapes:

| Shape | Example | `states` |
| --- | --- | --- |
| Two bits | `{ on: { table: holding, register: 60, bit: 4 }, off: { table: holding, register: 60, bit: 5 } }` | Integer keys: the raw code is on bit × 2 + off bit (section 1.2). Default `{ 0: 0, 1: 1, 2: 2, 3: 3 }`: off set alone is 1 (open), on set alone is 2 (closed) |
| Three bits | `{ on: {...}, off: {...}, fault: {...} }` | As two bits; a set fault bit gives 3 |
| Two coils or discrete inputs | `{ on: { table: discrete, register: 20 }, off: { table: discrete, register: 21 } }` | As two bits |
| A 2-bit field | `{ table: holding, register: 61, field: [0, 1] }` | Required: raw 0 to 3 to the four codes |
| An integer | `{ table: holding, register: 62, type: uint16 }` | Required |

### A.3 Device addresses on a channel

Reference 12.5 says "no overlapping or duplicate device addresses on a
channel". For Modbus this contract reads a device's address as the set of
`(unit id, table, register)` its points read or write. Open: needs the product
owner's ruling (X-4).

- Two devices on one channel may share a unit id when their sets are
  disjoint: slices of one gateway unit, each a device of its own. Overlap
  is CF-601. Each is demoted on its own failures; the driver counts failures
  per (channel, unit id) for devices that share one, so that a dead gateway
  unit does not cost each slice its own time-outs (driver interface
  contract).
- One device may answer on several unit ids: its default `unit_id` and the
  `unit_id` of some point addresses.
- Within one device, two points may not use the same register of one table
  for different values (CF-602), except that bits and bit fields of one
  register, and a register and its bit points, may share it, and that
  several write-only (`wo`) points may share a command register when the
  bits each can write are disjoint (A.5): they write, they do not read, so
  they do not overlap.
- The same data in two tables (a register readable as bits and as a register)
  is a template choice; the driver never merges across tables.

### A.4 Read rules

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `max_registers` | 1 to 125 | 125 | Registers per read |
| `max_bits` | 1 to 2,000 | 2,000 | Bits per read |
| `max_gap` | 0 to 124 | 0 | Unrequested registers a block read may bridge |
| `gap_policy` | `illegal`, `reads_zero`, `reads_sentinel` | `illegal` | What the device does with an unmapped register: an exception, zeros, or a marker. With `illegal`, `max_gap` must be 0 (CF-208); bridged registers are never decoded |
| `max_write_registers` | 1 to 123 | 123 | Registers per write |
| `input_as_holding` | bool | `false` | The input table is the holding table (for function substitution) |
| `ranges` | list of ranges | none | Below |

A **range** is `{ kind, table, first, last, unit_id }` (registers, with
`address_base`; `unit_id` optional, default the device's):

| `kind` | Meaning |
| --- | --- |
| `side_effect` | Reading these registers changes the device (latch bits that clear, a pointer that moves). Every point decoded from the range is served by one read of it per poll, so all must share a poll group (CF-610). The range is never retried after a lost answer, never read by `READ_NOW`, a keep-alive, an identity check or a demotion probe, and never split. A status word that clears on read and also qualifies values (`quality_from`) is one such range; the points that use it read it in the same read |
| `exclusive` | Read alone, in a request of its own, never merged with anything else (a record pointer). Implies everything of `side_effect` |
| `atomic` | Always read in one request and never split; a multi-register value is always atomic |
| `no_read` | Never read, also not inside a block (an event log the driver must not disturb) |

Ranges of different kinds must not overlap (CF-610). The block plan the
driver makes from these rules is its own (driver interface contract), and
shown in its diagnostics.

### A.5 Write encodings

`write` on a point with `access: rw` or `wo`. The value written is the raw
value of section 7.2, unless a key below says otherwise.

| Key | Type | Meaning | Example |
| --- | --- | --- | --- |
| `function` | `5`, `6`, `15`, `16`, `auto` | The function. `auto` (default): 5 for one coil, 6 for one register (16 if the device lacks 6), 16 for more | `{ function: 16 }` for a device that accepts only 16 |
| `value` | raw pattern | A fixed value written whatever is commanded, for "write anything to act" registers. With `trigger`, `pulse` | `{ value: 1 }` on a reset register |
| `values` | map from commanded value to raw pattern | The raw value per commanded value: inverted polarity, codes | `{ values: { true: 0, false: 1 } }`; for `double`: `{ values: { 1: 0x0002, 2: 0x0001 } }` |
| `on`, `off` | `{ table, register, value }` each | Separate registers or coils for the two directions (`double`, `single`, `step`); the commanded value selects one, and its `value` is written | `{ on: { table: holding, register: 501, value: 0xFF00 }, off: { table: holding, register: 502, value: 0xFF00 } }` |
| `mask` | raw pattern | For a whole-word command register: the bits this point sets; the other bits are written 0. Never a read-modify-write, never a stale image sent again. Several outputs may share one command register if the bits each can write are disjoint: a `mask` point writes its mask, a `values` point the patterns of its map, and the union of one point's patterns must not meet another's (CF-602) | `{ function: 6, mask: 0x0010 }` writes 0x0010 for `true`; `false` is refused (an `on_only` output) |
| `type` | raw type | The raw type written, when it differs from the type read | `{ type: int16 }` |
| `quantity` | 1 to 123 | Registers the device expects in the request, when it is more than the value's own; the rest are written 0 | `{ quantity: 2 }` |

- `value`, `values`, `on` and `off`, and `mask` exclude each other (an
  exclusive group, section 4.5).
- A coil target of a `single` output is BOOL. The target of a `double`
  output is INT64 with the codes 1 and 2 (section 7.2), also when its
  address is two coils with `on` and `off`; its `value_type` is derived as
  `int64` from the kind, so no override is written.
- A whole-word write of one point writes 0 to the bits of the other points
  that share the register. Whether writing 0 to a bit cancels what that
  bit holds is a property of the device, and the template author must know
  it.
- Example of a shared command register: a breaker as `values: { 1: 0x0001,
  2: 0x0002 }` and a reset as `mask: 0x0010` on the same register are
  allowed; a second point with `mask: 0x0002` is CF-602.
- Device-side select (roles select, operate, cancel on separate coils) and
  commands with a password register are reserved: they need the ruling on
  X-1 and on sessions with secrets (O-9).

### A.6 Codes of the Modbus `configure`

| Code | Rule | Message |
| --- | --- | --- |
| CF-650 | A read that cannot be planned within `max_registers`, `max_bits` and the ranges (an `atomic` range larger than `max_registers`) | `device {name}: range {range} cannot be read in one request` |
| CF-651 | A function that the address needs and `functions` excludes, with no substitution | `point {name} needs function {fn}` |
| CF-652 | A write encoding that does not fit the target's table (a `mask` on a coil, a `value` wider than the register) | `point {name}: {reason}` |
| CF-653 | A `side_effect` or `exclusive` range read by an identity check or a keep-alive request | `device {name}: range {range} cannot be read by {use}` |

## Annex B. The `io` protocol schema

Decision D-101. The protocol keys of `drivers/io/*.yaml` for `scada-io` (reference 6.5, #107).
The hardware abstraction layer resolves every name; no path is written in
the tree, so that no distribution-specific path appears in configuration
(`CLAUDE.md`).

### B.1 Channel keys

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `backend` | `simulator`, `gpio`, `iio` | required | Reference 6.5. Vendor backends are added as further values |
| `chip` | text | required for `gpio` | The GPIO chip by its label, as the kernel reports it, never by `gpiochipN` (the number can change between boots) |
| `device` | text | required for `iio` | The IIO device by its name, never by `iio:deviceN` |
| `scan` | `auto`, `every_cycle`, `helper` | `auto` | `every_cycle`: read and written by the scan thread in every cycle; only for lines whose calls do not sleep (a memory-mapped controller). `helper`: read and written by a helper thread that hands samples to the scan and takes its writes, safe-state writes included, one helper cycle later (driver interface contract); for GPIO expanders on I2C or SPI, whose calls sleep or take milliseconds. `auto`: `scada-io` chooses at run time from the bus the chip sits on: `every_cycle` only when the chip and every ancestor device are on the platform bus (memory-mapped); `helper` when any ancestor is on another bus (I2C, SPI, USB, a multi-function device on such a bus). An IIO channel is always read through the helper in phase 2 |

The kernel does not report whether a chip's calls sleep, so validation cannot
know it and does not depend on the hardware present: an explicit `scan:
every_cycle` is CW-660. At run time a channel whose calls on the scan thread
are repeatedly slow is moved to the helper, with a diagnostic, by the rule of
the driver interface contract (13.1): three calls above 1 ms within 1,000
cycles, and only while the scan thread runs under `SCHED_FIFO`; otherwise
the slow calls are counted and alarmed and the channel is never moved. The
move lasts until `scada-io` restarts; it is not stored.

Validation does not look for the lines, chips and IIO channels named: the
check mode opens nothing (13.1). A name that is not present on the unit is
found when `scada-io` requests the line: that channel fails (its points
COMM_FAIL, a diagnostic tag and a log record), the tree is not refused, and
the other channels run.

### B.2 Points

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `line` | text or 0 to 1,023 | required for `gpio` | The line by its name, or its offset on the chip |
| `direction` | `input`, `output` | required for `gpio` | |
| `active_low` | bool | `false` | |
| `bias` | `none`, `pull_up`, `pull_down` | `none` | |
| `debounce` | duration, `0ms` to `1s` | `0ms` | Digital inputs (reference 6.5) |
| `at_start` | `hold`, `safe` | required for an output | A line requested as an output must be given a level when it is requested. `hold`: the line is first requested as it is (no direction given, so a driven output keeps being driven), its level read, and then kept as an output at that level; a line found as an input (the first start after a boot) becomes an output at the level read. `safe`: it is requested at the output's `safe_state`. Reference 14.4's "not driven until one full control cycle" means `hold` here |
| `channel` | text | required for `iio` | The IIO channel by its name (`in_voltage0`) |
| `filter` | duration, `0ms` to `60s` | `0ms` | First-order filter time constant of an analog input (reference 6.5) |
| `clamp` | `[min, max]` | none | Clamping of an analog value after scaling (reference 6.5) |

Scaling uses the common `scale` of section 4.7. Phase 2 reads single IIO
values only, through the helper; buffered capture and IIO analog outputs
need set-up that the service user cannot do, and are reserved in both
contracts.

### B.3 Simulator

`backend: simulator` has the same point keys and a `profile` per point
(`constant`, `ramp`, `sine`, `square`, `random`, with their parameters), so
that every other component can be tested without hardware (reference 6.5).

### B.4 Outputs at a crash

- The kernel releases a GPIO line when the descriptor that holds it closes
  and promises no level afterwards: most controllers keep the last level,
  some do not. A crash of `scada-io` therefore leaves its local outputs as
  the hardware leaves them; the process that died cannot drive a safe state.
- After every stop of `scada-io`, a crash included, the service manager runs
  a small stop step (#107 builds it, from the expanded file of section
  13.3). It acts on the same outputs in both contracts: an output whose
  `on_shutdown` is `safe` is requested at its `safe_state`, one whose
  `on_shutdown` is `release` at `limits.release`, and an output with `hold`
  is not touched. Then it releases the lines and exits.
- **After the stop step nothing is promised.** The kernel promises no level
  for a released line. A safe level that must stay while no process runs
  is a deployment requirement: the output is on a controller that keeps
  its last level after release, or has a hardware pull to its safe level.
- The stop step reads the expanded file whose `config_hash` is that of the
  `current` generation, which survives a crash of `scada-io` in shared
  memory (13.5, "the expanded file of a generation"), and the active
  version's only when there is no generation. So it drives the outputs
  `scada-io` was running, not an output a newer, not yet applied version
  adds; an output removed by an applied commit is not driven.
- The service manager runs the step after every stop, also an orderly stop
  (the outputs are then already in their state; writing it again is
  harmless) and a failed start. It exits 0 when `scada-io` has no channel,
  and has a time bound of its own of 5 s (a bus call can take the adapter's
  time-out). It does not run on a power cut, a kernel fault or a watchdog
  reset: that is the hardware default's case (reference 14.4).
- A hung system is covered by the hardware watchdog and the outputs'
  hardware default (reference 14.4).

### B.5 Codes of the `io` `configure`

| Code | Rule | Message |
| --- | --- | --- |
| CW-660 | `scan: every_cycle` given explicitly (B.1) | `channel {name}: every_cycle is only for a controller whose calls do not sleep` |
| CF-662 | Two points on one line | `points {a} and {b} use line {line}` |

## Annex C. Keys with several shapes

Every key that takes more than one shape, with its shapes, so that the JSON
Schema files of #100 list them (`oneOf`) and a reader finds them in one
place.

| Key | Shapes |
| --- | --- |
| `logging` | a policy name; `none`; `{ policy, <settings> }`; `{ <settings> }` complete (section 6) |
| `feedback` | a tag reference; `{ on, off }` of two BOOL tags; `{ tag, within, settle }` (7.2) |
| `safe_state` | `off`; `on`; `hold`; `{ value }`; `{ raw }` (7.2) |
| `limits.release` | `{ value }`; `{ raw }` (7.2) |
| `interlocks` | a list of interlock references; `{ on: [...], off: [...] }` (7.2) |
| A bit definition (`bits`) | a name; `{ name, invert, description, alarms, logging, retain }` (5.4) |
| `invalid_values` | in a point: a list of patterns or `{ raw, action, flags, reason }`; in `defaults`: a map from raw type to such a list (4.7) |
| `on_invalid`, `action` | `bad`; `keep_last`; `{ substitute: <value> }` (4.7) |
| `alarm_on` | a value; a list of values; enumeration names (8) |
| `blocks[].in` sources | a tag reference; `<block>.<pin>`; a literal; `{ tag, max_age }`; `{ from, prev: true }` (9.2) |
| `good_when` (health) | a value; a list; `{ field, equals }` (4.9) |
| `identify` entries | `{ address \| point, equals \| in \| range \| pattern }` (4.8) |
| `counter` | `{ limit, at_limit, on_decrease }` (5.3) |
| `states` | a map from integer raw code to 0 to 3 (5.3; for bit shapes the code is on bit × 2 + off bit, A.2) |
| `restart_indication` entries | `{ point, rule: decreases \| changes \| { equals } }` (4.9) |
| Double-point address | two bits; three bits; two coils; a field; an integer (A.2) |
| `write` (Modbus) | `function`, and one of `value`, `values`, `on` and `off`, `mask`; with `type`, `quantity` (A.5) |
| `enum` | an enumeration reference; an inline map (5.5) |
| `scale` | `{ gain, offset }`; `{ divide_by, offset }` (4.7) |
| `device_time` | `{ zone: utc, trust }`; `{ zone: local, tz, trust }` (4.6) |
| `keepalive` | a duration; `off` (4.6) |

**The JSON Schema files** (D-69) use the 2020-12 dialect, because merging
framework and protocol keys into one object with "a key in neither is
CF-201" needs `unevaluatedProperties`, which `additionalProperties` does not
give across composed schemas. The validator is the project's own, over a
parsed tree that keeps every value's file, line and column, with a fixed
subset of keywords (`type`, `enum`, `const`, `properties`, `required`,
`unevaluatedProperties`, `items`, `minItems`, `maxItems`, `minimum`,
`maximum`, `pattern`, `oneOf`, `allOf`, `$ref`, `$defs`), each mapped to a
CF-2xx code. A general validator library reports neither locations nor these
codes and would be a new dependency. The schemas check files before
expansion (expression strings where numbers are expected); everything after
expansion is code.
