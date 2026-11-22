# Contract: configuration schemas

| | |
| --- | --- |
| Contract version | 0.1 (draft) |
| Tree format | 1 (section 11) |
| Status | Draft for review (issue #98, P2-01). No code for P2-03 to P2-06 starts before a person has reviewed it. |
| Implements | Reference architecture 1.1, sections 12, 7.3, 7.4, 7.6, 8, 9.1, 10.1, and the configuration lines of 4.3, 6, 14.4 and 17.7 |
| Agrees with | Tag database contract 1.0 (sections 3, 5, 12, 14 and 14.1); driver interface contract (`driver-interface.md`, #99, drafted at the same time) |
| Owners of this contract | `scada-conf`, `libscada-conf` |

This contract fixes the configuration tree that people write by hand under
`/etc/scada/`. It has three parts:

- what each file may contain: the keys, their types, defaults and limits,
  written as tables (sections 3 to 9);
- the rules a schema cannot express: template expansion, the merge order,
  name rules, the expression grammar, cross-file validation with error codes
  (sections 4, 5 and 10);
- how the tag-set definition of the tag database and its `config_hash` are
  computed from a valid tree (section 12).

The tables are normative. P2-03 (#100) turns them into JSON Schema files, one
per file type, under `libs/scada-conf/schemas/`. When a table and a schema
file disagree, the table wins and the file is fixed (D-69).

The words **must**, **should** and **may** are used as in the reference
document. "Phase 2" means what P2-03 to P2-06 build. A key or value marked
**reserved** is part of the format but refused by the validator of this
version (error CF-210), so a later version can accept it without a format
change (section 11). A place marked **open: needs the product owner's
ruling (X-n)** is drafted with the option this document proposes, and stays
open until the owner rules. X-n refers to the list of conflicts in the device
survey of 2026-11-22 (not in the repository). The pull request of this
version lists every such ruling with its options.

Device facts quoted in this document come from the device survey of
2026-11-22 (not in the repository) and are described generically. Every
address, register number and name in the examples is invented.

## Contents

1. File tree
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
13. Conformance tests
14. Open rulings
15. Change log

Annex A: the Modbus protocol schema.

## 1. File tree

The tree is the set of files below the configuration root, `/etc/scada/` for
the active tree, a staging directory for a staged one (reference 12.4).
`scada-conf` validates a whole tree, never a single file alone.

| Path | Schema | Content | Section |
| --- | --- | --- | --- |
| `system.yaml` | `system` | Unit identity, node role, time, network, storage, upstream, email, limits, data plane. Required | 3 |
| `areas.yaml` | `areas` | Areas and their control authority | 7.1 |
| `templates/<name>.yaml` | `template` | One device template per file. Subdirectories are allowed, at most 3 levels deep | 4 |
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
  in phase 2. A directory under `drivers/` with another name is CF-101.
- `<channel>` is the channel's name, and the file name must equal the
  `channel.name` key (CF-102). `<name>` of a template file must equal its
  `template` key (CF-102). A file name is the name plus `.yaml`, so a lookup
  needs no search.
- A reserved file, or any file not listed above, is CF-101. Names that start
  with `.` are not exempt: an editor's backup file in the tree is an error,
  so that what is validated is exactly what is committed.
- User accounts, password hashes and secret values are never in the tree
  (reference 12.1, 13.3). A secret is referenced by name (section 2.6).

### 1.1 The YAML subset

Decision D-70. A file must be YAML 1.2 in this subset. Anything outside it is
CF-110, with the line and column of the first offending token.

| Allowed | Not allowed |
| --- | --- |
| UTF-8 without a byte order mark; LF or CRLF line ends | Other encodings; a NUL byte; other control characters outside strings, except tab in comments |
| One document, without `---` or `...` markers | Several documents; directives (`%YAML`, `%TAG`) |
| Block and flow mappings and sequences | Anchors (`&a`), aliases (`*a`), merge keys (`<<`) |
| Keys that are plain or quoted strings | Complex keys (`? `), non-string keys, duplicate keys (CF-111) |
| Plain, single-quoted and double-quoted scalars; literal (`\|`) and folded (`>`) block scalars | Tags (`!x`, `!!str`) |
| Comments (`#`) | `null`, `~` or an empty value; a key must have a value (CF-112) |
| `true` and `false` | `yes`, `no`, `on`, `off`, `True` (strings in YAML 1.2, refused where a boolean is expected) |
| Integers: decimal with an optional sign; hexadecimal `0x` with digits `0-9a-fA-F` | Octal (`0o17`), binary, leading zeros (`010`), `_` separators |
| Decimal numbers with `.` or an exponent | `.inf`, `.nan` (written as the strings `inf`, `-inf`, `nan` where a key allows them) |

Limits, each a CF-113 when exceeded: a file of at most 8 MiB; nesting depth
at most 32; a scalar of at most 64 KiB; a key of at most 64 bytes; at most
20,000 files and 256 MiB in a tree. A generated template of several thousand
points fits.

JSON files (screens, phase 8) are read with the same parser: JSON is a subset
of YAML 1.2, and the limits above apply.

Reason: anchors, merge keys and tags let one line change values far away and
make "file and line" of an error ambiguous; templates (section 4) are the
mechanism for reuse. Refusing `null` and the YAML 1.1 booleans removes the
values people type by accident.

### 1.2 Errors and their location

Every error and warning names a file, a line and a column (1-based), a code
and a fixed message (section 10). A value that came from a template names the
instance first and then the template file and line it came from (#103):

```
drivers/modbus/line1.yaml:14:7: error CF-402: tag name ps1.pump_station_number_one.fm1.flow_total_since_commissioning is 131 bytes, the limit is 127
  from template templates/flow_meter_x.yaml:22:9 (point flow_total_since_commissioning)
```

A tree with an error is not committed (reference 12.4). A tree with warnings
only can be committed; the warnings are shown and kept with the commit.

## 2. Common definitions

### 2.1 Names

| Name | Grammar | Length | Examples |
| --- | --- | --- | --- |
| Tag name | Segments joined by `.`; a segment is `[a-z0-9][a-z0-9_-]*` | 1 to 127 bytes (tag database contract 14), after expansion (section 4.5) | `ps1.pump2.flow`, `sys.drv.line1.state` |
| Point name (in a template) | One or more segments, as a tag name | 1 to 127 bytes; the full tag name is checked | `flow`, `meter.power`, `pv3.voltage` |
| Device name | As a tag name. It is the prefix of the device's tags | 1 to 126 bytes | `ps1.fm1` |
| Identifier | `[a-z][a-z0-9_]*` | 1 to 31 bytes | template, policy, area, program, block, alarm, parameter, enumeration and interlock names |
| Channel name | `[a-z][a-z0-9_]*`, unique across the unit | 1 to 31 bytes | `line1` |
| Task name | `[a-z][a-z0-9_]*` | 1 to 31 bytes | `fast`, `normal` |
| Poll group name, tier name | `[a-z0-9][a-z0-9_]*` | 1 to 8 bytes | `fast`, `1m`, `15m` |
| Unit id | `[a-z0-9][a-z0-9-]*` | 1 to 31 bytes | `site-0042` |

- Tag names are unique across the unit (reference 12.2). A user tag name must
  not start with the segment `sys`, which is kept for generated system tags
  (CF-301).
- Owner and queue names of the tag database are at most 31 bytes; they are
  generated from channel and task names by the rule of section 12.3, so the
  names users write are not shortened for them (D-75).
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
- Decimal numbers are read as the nearest binary64 value. Where exactness
  matters (scaling), a key offers an integer form (`divide_by`, section 4.7).
- A value for a tag (a safe value, a substitute, a limit) must fit the tag's
  type: a BOOL takes `true` or `false`, an INT64 or UINT64 an integer in
  range, a FLOAT64 any number.

### 2.4 Configuration expressions

Some keys of templates take an **expression** over constants and template
parameters, evaluated once during expansion (section 4): `when`, addresses,
`scale`, `repeat` ranges. Such a key accepts either its literal type or a
string, and a string is an expression. The grammar is that of section 5.6
without tag references or time functions; names refer to parameters and to
`repeat` variables. An expression that refers to an unknown name, has the
wrong type, divides by zero or overflows is CF-410.

```yaml
when: "has_meter"
scale: { gain: "ct_primary / 1000" }
address: { table: holding, register: "100 + 2 * (n - 1)", type: int16 }
```

### 2.5 Patterns

A pattern matches tag or point names: `*` matches any characters within one
segment, `?` exactly one character of a segment. `ps1.inv*.power` matches
`ps1.inv1.power` and `ps1.inv12.power`, not `ps1.inv1.dc.power`. Patterns
are resolved during validation, never at run time. A pattern that matches
nothing is CF-311.

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
anywhere is CF-214 ("secrets are not available in this version"). The
driver receives the name, not the value, and resolves it through the store
when one exists.

## 3. `system.yaml`

```yaml
format: 1
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
| `format` | integer | required | Tree format, 1 for this contract (section 11) |
| `unit.id` | unit id | required | Identity of the unit: audit, upstream, node link |
| `unit.name` | text, at most 64 bytes | required | Display name |
| `node.role` | `standalone`, `master`, `slave` | `standalone` | Reference 17.1. `master` and `slave` are reserved until phase 11 |
| `time.timezone` | IANA zone name | required | For display and schedules only; every stored time is UTC (reference 12.2) |
| `time.sync.mode` | `ntp`, `ptp`, `none` | `ntp` | Reference 12.3 |
| `time.sync.servers` | list of host names or addresses, 1 to 8 | required unless `mode: none` | |
| `time.sync.alarm_offset_ms` | 1 to 60,000 | 500 | System alarm above this offset |
| `time.sync.alarm_unsynced_s` | 10 to 86,400 | 600 | System alarm when not synchronised for this long |
| `network.hostname` | host name | the unit id | Applied by `scada-sys` (reference 12.6) |
| `network.interfaces[]` | list | none | `name` (interface name), `zone` (`field` or `management`, reference 13.6), `addresses` (list of CIDR), `gateway`, `dns` (list) |
| `storage.free_space_alarm` | percentage | `10%` | Reference 9.4 |
| `storage.journal_reserve` | size | `2GiB` | Space kept for journals and configuration (reference 14.4) |
| `upstream` | map | absent: no upstream | `url` (HTTPS), `auth` (`certificate` or `token`, each a secret), `order` (`newest_first` or `oldest_first`, default `newest_first`), `rate_limit` (size per second), `window` (`from`, `to` as `HH:MM` in the unit's zone). Used from phase 10 |
| `email` | map | absent: no email | `server`, `port` (default 587), `tls` (`starttls` or `implicit`, default `starttls`), `username`, `password` (secret), `from`. Used from phase 7 |
| `limits.max_tags` | 1 to 1,000,000 | 50,000 | Tags of the tag set, generated tags included (CF-1001). The tag database limit is 1,000,000 |
| `limits.max_devices` | 1 to 10,000 | 1,000 | Devices of the unit (CF-1002) |
| `limits.max_devices_per_channel` | 1 to 256 | 247 | CF-1002 |
| `data_plane.heartbeat_check_ms` | 1 to 60,000 | 250 | How often `scada-tagd` checks heartbeats and retain files; written into the tag set (section 12). **In a tag set with a `retain_now` tag, `heartbeat_check_ms` must be at most 400 ms, so that a RETAIN_NOW change is on disk within 1 s: two checks plus a write of 200 ms fit in it, (1000 − 200) / 2** (tag database contract 12, D-62). A larger value is refused with "heartbeat_check_ms must be at most 400 in a tag set with a retain_now tag" (CF-1010). Every tree with an output or an area has `retain_now` tags (section 12.5), so in practice the limit always applies |
| `data_plane.retain_interval_ms` | 1,000 to 60,000 | 5,000 | Retain interval of `scada-tagd` (`--retain-interval-ms`, tag database contract 12). Not in the tag set |
| `data_plane.generation_change_limit` | duration, `1s` to `60s` | `5s` | Time limit of the coordinated restart (reference 5.5, 19.2) |
| `data_plane.processes.<process>.heartbeat_timeout_ms` | 1 to 600,000 | 3,000; 500 for a real-time process | Per process (section 12.2) |
| `data_plane.change_ring_capacity` | power of two, 1,024 to 1,048,576 | 65,536 | Default change ring capacity per owner (section 12.3) |
| `data_plane.event_queue_capacity`, `data_plane.command_queue_capacity` | power of two, 16 to 1,048,576 | 1,024 | Capacity of each event queue and of each command and result queue |

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
unchanged:

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
  defined by the protocol's address schema (Annex A for Modbus) and is
  opaque to the rest of this contract. `scada-conf` validates it against that
  schema, and the protocol validates it again at `configure` (driver
  interface contract).
- Channel and device objects follow the same rule: the framework keys of
  this contract and the protocol keys of the protocol's schema stand side by
  side in one object. The two key sets are disjoint, checked when the
  schemas are built; a key in neither is CF-201.
- A template has at most one `protocol`. A template without `protocol` is a
  **base template**: common parts only, no `address`, no `read`, no
  `identify` (CF-421). A device type that speaks several protocols is a base
  template with the signals, and one template per protocol that `extends` it
  and adds the addresses, the scaling and the validity rules, which differ
  between protocols for the same signal.
- Room for (a): a later version may accept, in one template, an `addresses:`
  map from protocol name to address per point. It is reserved (CF-210).

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
| `defaults` | point object without `name` | none | Merged under every point (section 4.5, step 5). May hold a partial `address` (for example `word_order`) |
| `enums` | map of identifier to enumeration | none | Local value tables (section 5.5) |
| `points` | list of point objects | none | Section 4.7 |
| `repeat` | list of repeat blocks | none | Section 4.4 |
| `read` | protocol read rules | protocol default | Annex A.4 for Modbus |
| `identify` | list of identity checks | none | Section 4.8 |
| `health` | list of health rules | none | Section 4.9 |
| `on_mismatch` | `warn`, `refuse` | `refuse` | What an identity mismatch does (section 4.8) |
| `symbol` | identifier | none | Default symbol, phase 8 |
| `session` | reserved | | Steps at connect and restart with secrets (open: further question 1 of the survey; room only) |

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

### 4.4 Parameters, `when`, `disable` and `repeat`

```yaml
params:
  inputs:    { type: int, min: 1, max: 32, default: 16 }
  has_meter: { type: bool, default: false }
  options:   { type: set, values: [battery, meter] }
points:
  - name: meter.power
    when: "has_meter"
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
  range is CF-412. A `set` value is a list of its `values`; `x in options` is
  true when `x` is in the list.
- `when` on a point, or on a repeat block, is a configuration expression of
  type BOOL. A point whose `when` is false does not exist: it creates no tag
  and is not read. Use it for options, cards, models and firmware variants,
  above all where an absent option reads as 0 instead of failing.
- An instance's `disable` lists point names or patterns to remove. A name that
  matches no point is CF-413, so that a typing error does not leave a point
  active.
- A **repeat block** has `for`, a map of one or two variables to ranges, and
  `points`. A range is `"a..b"` (inclusive, integers, from expressions) or a
  list of integers and ranges, for irregular index sets: `[1, 2, "5..8"]`.
  For two variables the points are produced for every pair, the first
  variable outermost. Inside the block, `{n}` in a point name is replaced by
  the value of `n`, and `n` may be used in every expression. At most 65,536
  points per template after expansion (CF-414).
- Irregular address layouts (a run that continues at another base, a reserved
  register inside a run) are written with `if(...)` in the address
  expression, or as several repeat blocks with their own ranges.

### 4.5 Expansion and the merge order

Decision D-72. An instance is expanded into its final list of points in these
steps, in this order. Every merged value keeps the file and line it came
from, so an error names the instance and the template line (section 1.2).

1. **Chain.** The `extends` chain is resolved, from the root template down to
   the instance's template.
2. **Parameters.** The parameter declarations of the chain are collected
   (a child adds parameters, or changes the `default`, `min` or `max` of an
   inherited one, never its `type`, CF-415), and the instance's `params`
   replace the defaults. This comes before the levels, because `when` and
   `repeat` of every level need the values.
3. **Levels.** For each template of the chain, from the root down:
   1. `defaults`, `read`, `enums` (by name; a child's table replaces the
      inherited table of the same name whole): merged into what the levels
      above gave;
   2. its `repeat` blocks are expanded, with the values of step 2 and the
      variables;
   3. its points, those written and those from `repeat`, are merged by name
      into the inherited points: a point of the same name is merged into the
      inherited one, the child's keys winning; a new name is added. Two
      points of one name within one level are CF-416;
   4. `identify` and `health`: a level that gives the key replaces the
      inherited list whole.
4. **Presence.** Every point whose `when` is false is removed, then every
   point the instance's `disable` matches.
5. **Defaults.** The template's `defaults`, merged with the instance's
   `defaults` (instance winning), are merged under every point: a key the
   point gives wins.
6. **Overrides.** The instance's `overrides` (a map from point name to a
   partial point) are merged into the points, the override winning. An
   override of a point that does not exist, or that step 4 removed, is
   CF-413.
7. **Prefix.** Each point's tag name becomes `<device name>.<point name>`;
   generated tags of the point (bits, section 4.7) get theirs. Names are
   checked (CF-402, CF-403).
8. **Validation** of the expanded tree, with every rule of section 10.

The merge of two values: two maps are merged key by key, recursively; any
other value (a scalar, a list) replaces the one before it whole. There is no
way to delete a key, only to replace its value; points are removed by
`when` and `disable`. Lists are replaced whole so that the result of a merge
does not depend on positions in two files. An alarm list of a point is
therefore replaced whole by an override that gives `alarms`.

An instance that names no template gives `points` directly (reference 12.1,
"or lists its points"); steps 1 to 3 are then empty, and its `params` are refused (CF-201). An instance may give
`points` and a template together: its points are added after step 6 and a
name that already exists is CF-416.

### 4.6 Channel files and device instances

```yaml
# drivers/modbus/line1.yaml
channel:
  name: line1
  type: rtu
  port: /dev/ttyS1
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
| `name` | channel name | required | Equals the file name |
| `enabled` | bool | `true` | A disabled channel is not connected. Its tags and system tags stay in the tag set (BAD, NOT_INIT), so that enabling or disabling it is a live change and references to its tags stay valid. The same holds for a disabled device (driver interface contract, live reload) |
| `role` | `connect`, `listen` | `connect` | Who opens the connection. `listen` is reserved (open: needs the product owner's ruling (X-2)). Both are the client or master role of reference 6.4 |
| `poll_groups` | map of poll group name to `{ period, priority }` | `fast` 200 ms, `normal` 1 s, `slow` 10 s, priorities 1, 2, 3 | Reference 6.2. `period` 10 ms to 1 d; `priority` 1 (highest) to 16 |
| `min_request_interval` | duration, `0s` to `60s` | `0s` | Pacing of the channel: least time between two requests on it |
| `keepalive` | duration or `off` | `off` | Send a harmless request when the channel has been idle this long, for devices that close idle sessions |
| `reconnect` | `{ min, max }` durations | `1s`, `60s` | Back-off of reconnects after a connection failure |

Framework keys of a device instance:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | device name | required | Prefix of every tag of the device; unique across the unit |
| `template` | template reference | none | Section 4.5 |
| `params`, `defaults`, `overrides`, `disable`, `points` | | none | Section 4.5 |
| `enabled` | bool | `true` | As for a channel |
| `timeout` | duration, `10ms` to `60s` | `1s` | Response time-out of one request |
| `write_timeout` | duration, `10ms` to `60s` | `timeout` | Time-out of a write request; `write_timeout_per_value` adds to it for each value written |
| `write_timeout_per_value` | duration, `0s` to `10s` | `0s` | For devices whose write time grows with the number of values |
| `retries` | 0 to 10 | 2 | Retries of a failed read |
| `demote_after` | 1 to 100 | 3 | Consecutive failed requests before the device is demoted (reference 6.2) |
| `demoted_retry` | duration, `1s` to `1h` | `30s` | Retry period of a demoted device |
| `startup_grace` | duration, `0s` to `10m` | `0s` | After a connect or a detected restart, time-outs in this time do not count toward demotion |
| `min_request_interval` | duration, `0s` to `60s` | `0s` | Pacing of the device: least time between two requests to it, on top of the channel's |
| `min_write_interval` | duration, `0s` to `60s` | `0s` | Least time between two writes to the device |
| `max_values_per_request` | 1 to 65,535 | protocol limit | Most values one read or write may carry |
| `acquisition` | `poll`, `report` | `poll` | `report` is reserved for the report-driven protocols of phase 9; their report settings are protocol keys |
| `device_time` | `{ zone: utc \| local, tz }` | `{ zone: utc }` | How the device's time values are read: `local` with an IANA zone `tz`, converted to UTC by the driver |
| `time_sync` | `{ mode: none }` | `none` | Who sets the device's clock. Other modes (`on_connect`, `periodic` with `period`) are reserved (open: needs the product owner's ruling (X-12)); the device must not be set from a unit whose own time is not synchronised |
| `on_mismatch` | `warn`, `refuse` | the template's | Section 4.8 |
| `identify` | list | the template's | Section 4.8; an instance may add checks (a serial number) |

- Several devices may share one connection; each has its own time-out,
  retries and demotion (driver interface contract).
- How several devices on one channel may share or split a device address is
  the protocol's rule (Annex A.3 for Modbus).

### 4.7 Point keys

The keys of the common part. Keys that apply only to some kinds are marked.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | point name | required | |
| `value_type` | `bool`, `int64`, `uint64`, `float64`, `string` | derived (below) | Type of the tag |
| `kind` | `plain`, `counter`, `double_point` | `plain` | Section 5.3 |
| `description`, `label`, `unit` | text | none | `label`: section 2.1 |
| `range` | `[min, max]` numbers | none | Engineering range: percentage deadbands, display |
| `raw_range` | `[min, max]` numbers | none | Raw values outside it set OUT_OF_RANGE, for devices that saturate without saying so |
| `access` | `ro`, `rw`, `wo` | `ro` | `rw` and `wo` may be the target of an output (section 7.2). A `wo` point is never read; its tag shows the last value written (driver interface contract) |
| `side_effect` | bool | `false` | Reading it changes the device (clears a flag, moves a pointer). Never merged into a block, never retried, no `READ_NOW` (CF-610) |
| `poll_group` | poll group reference | `normal` | |
| `max_age` | duration, `10ms` to `1d` | 3 times the poll group period | Beyond it the tag gets STALE (reference 6.2). For reported points the meaning is that of the driver interface contract |
| `update_period` | duration | none | How often the device itself refreshes the value; for validation only (CF-611) |
| `scale` | `{ gain, offset }` or `{ divide_by, offset }` | none | value = raw × gain + offset, or raw ÷ divide_by + offset. `gain`, `offset` are numbers or configuration expressions; `divide_by` is a positive integer or an expression giving one |
| `exponent_from` | `{ point, direction: multiply \| divide }` | none | value × 10^e or ÷ 10^e, where e is the raw value of another point read in the same request (CF-612). An exponent outside −20 to 20, or invalid, makes the value BAD |
| `invalid_values` | list of entries | the template's `defaults` | Raw patterns that mean "no value" (below) |
| `on_invalid` | `bad`, `keep_last`, `{ substitute: <value> }` | `bad` | What an invalid raw value gives (below) |
| `quality_from` | list of quality rules | none | Quality from status bits (below) |
| `source_time_from` | point reference | none | The point whose value (a time, section A.2) is the source time of this one, read in the same request (CF-612) |
| `bits` | map of bit number to a bit definition | none | Section 5.4 |
| `tag` | bool | `true` | `false`: the point creates no tag of its own, only its bits or its role as a companion |
| `enum` | enumeration reference or inline map | none | Section 5.5 |
| `counter` | counter rules | none (`kind: counter` only) | Section 5.3 |
| `states` | map | protocol default (`kind: double_point` only) | Section 5.3 |
| `storage` | `volatile`, `nvm` | `volatile` | `nvm`: a write goes to the device's non-volatile memory, whose write count is limited (section 7.2) |
| `nvm_limits` | `{ min_interval, max_per_day }` | `{ min_interval: 1h, max_per_day: 10 }` | For `nvm` only. The driver framework refuses writes beyond them |
| `dangerous` | bool | `false` | Writing it has effects beyond the point (a reset that acts for every master, a factory reset, a test function). Section 7.2 |
| `change_latch` | point reference | none | Reserved: a latch bit that records a change between two reads; the behaviour is decided in the driver interface contract |
| `when` | configuration expression | `true` | Section 4.4 |
| `alarms` | list of alarms | none | Section 8 |
| `logging` | logging settings | not logged | Section 6 |
| `retain`, `retain_max_age` | | `false`, none | Section 5.2 |
| `address` | the protocol's address | required in a protocol template once expanded (CF-417) | Annex A for Modbus |

**Derived value type** (when `value_type` is not given): a bit, a coil or a
`bool` raw type gives `bool`; an integer raw type without `scale` and
`exponent_from`, or with an integer `gain` and integer `offset`, gives
`int64` (`uint64` for an unsigned raw type of 64 bits); any other scaling and
any floating raw type gives `float64`; a string raw type gives `string`;
`kind: double_point` and `enum` give `int64`. A FLOAT64 holds integers
exactly up to 2^53; a 64-bit counter scaled by a fraction is written as a raw
integer point plus a virtual tag. A `value_type` that cannot hold the raw
type is CF-504.

**Invalid values.** An entry is `{ raw: <pattern>, action, flags, reason }`.

- `raw` is compared with the raw bit pattern before decoding and scaling: an
  integer (decimal or hexadecimal, of the raw type's width, so a float32 marker
  is written as its 32-bit pattern, `0x7F800000`), or the string `nan` (any
  NaN of a floating raw type).
- `action` is `bad`, `keep_last` or `{ substitute: <value> }`, default the
  point's `on_invalid`. `bad` writes the tag BAD, with no COMM_FAIL, the
  `flags` given (any of OUT_OF_RANGE, OVERFLOW, BLOCKED, TEST), and does not
  count toward demotion. `keep_last` keeps the last value with UNCERTAIN and
  SUBSTITUTED. `substitute` writes the value with UNCERTAIN and SUBSTITUTED.
- The template's `defaults.invalid_values` may be a map from raw type to a
  list (`{ uint16: [0xFFFF], float32: [nan] }`), applied to every point of
  that raw type. A point's own `invalid_values` list replaces it whole, and
  `invalid_values: []` turns it off. There is no system-wide default: a
  pattern that is a marker on one device is a valid value on another.

**Quality rules.** An entry of `quality_from` is
`{ point, bit | field, map }`: the value of `point` (another point of the
template read in the same request, or `self` for the point's own raw word),
or of the given bit or bit field (`field: [lo, hi]`) of it, is looked up in `map`, whose values
are `good`, `uncertain`, `bad`, or a list of one of these and flags; `default`
covers values not listed. The worst result of all rules and of the value's
own checks is the tag's quality. A point used by a quality rule is read with
the points that use it (CF-612).

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
value of a `point` is compared with an integer, a list, `[min, max]` or a
pattern of text. With `on_mismatch: refuse` a failed check puts the device in
"configuration mismatch": its tags read BAD and it is not written. With `warn`
the device runs and the mismatch is a diagnostic and a system alarm. The
default is `refuse`, because a template applied to the wrong variant reads
plausible numbers from the wrong registers.

### 4.9 Health rules

`health` is a list of `{ point, good_when, applies_to, comm_fail }`: when the
value of `point` is not `good_when` (a value, a list, or `{ field, equals }`),
the tags in `applies_to` (`all`, or a list of point names and patterns) are
written BAD; with `comm_fail: true` they also get COMM_FAIL. Use
`comm_fail: true` for a device that is lost behind a gateway, `false` for a
device that reports a fault of its own. A health point is read before the
points it applies to in every cycle where both are due.

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
  `limit` (the raw value at which the device wraps or stops), `at_limit`
  (`wrap` or `saturate`, default `wrap`), and `on_decrease` (`accept`,
  `hold`, `reset`, default `accept`): `accept` writes the smaller value;
  `hold` keeps the last value with UNCERTAIN until the counter passes it again
  (for a device that steps back after a power cut); `reset` writes the value
  and records a counter reset in the journal. A saturated counter gets
  OVERFLOW. `logging: counters` in reference 12.3 is a policy name like any
  other.
- `kind: double_point` is a two-bit position with an INT64 tag that every
  driver writes with the same codes, so that `scada-cmd`, alarms and screens
  need one coding:
  `0` intermediate, `1` open or off, `2` closed or on, `3` faulty. Code `3` is
  written BAD; `0` stays GOOD, so a travelling switch is not a fault. The
  protocol's address gives the two bits, two coils, a 2-bit field or an
  integer; `states` maps the device's raw codes onto the four codes, because
  devices code the same position differently (a map is required where the
  protocol has no single coding, CF-613).

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
conditions and the structured text of section 9.3 use one expression grammar,
the expression part of the structured-text subset (reference 7.4, 7.6).

```
expression   = or_expr ;
or_expr      = xor_expr { "OR" xor_expr } ;
xor_expr     = and_expr { "XOR" and_expr } ;
and_expr     = cmp_expr { ( "AND" | "&" ) cmp_expr } ;
cmp_expr     = add_expr [ ( "=" | "<>" | "<" | "<=" | ">" | ">=" ) add_expr ] ;
add_expr     = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr     = pow_expr { ( "*" | "/" | "MOD" ) pow_expr } ;
pow_expr     = unary [ "**" pow_expr ] ;
unary        = [ "-" | "NOT" ] primary ;
primary      = literal | call | tag_ref | "(" expression ")" ;
call         = identifier "(" [ argument { "," argument } ] ")" ;
argument     = expression | string ;
tag_ref      = name_segment { "." name_segment } ;
name_segment = letter_or_underscore { letter | digit | "_" } ;
literal      = integer | real | "TRUE" | "FALSE" | time | "GOOD" | "UNCERTAIN" | "BAD" ;
time         = "T#" digits ( "ms" | "s" | "m" | "h" | "d" ) ;
string       = '"' { any character but '"' } '"' ;
```

- Keywords, function names and the literals are case-insensitive; tag names
  are lower case. A comparison does not chain (`a < b < c` is CF-520).
- A tag name with a segment that does not fit `name_segment` (a `-`, or a
  segment that starts with a digit) is written `tag("ps1.pump-2.flow")`.
- `x.quality` and `x.age` read the quality (`GOOD`, `UNCERTAIN`, `BAD`) and
  the age (TIME) of tag `x` (reference 7.4). If `x.quality` is itself a
  tag name, the reference is ambiguous and is CF-521; `quality(x)` and
  `age(x)` are the unambiguous forms.
- Types are `BOOL`, `INT` (64-bit), `REAL` (64-bit), `TIME` (milliseconds),
  as reference 7.4. A BOOL tag reads as BOOL, INT64 as INT, FLOAT64 as REAL; a
  UINT64 tag reads as INT and its value above 2^63 − 1 gives a BAD result
  with OVERFLOW. STRING tags cannot be used (CF-522).
- `INT op INT` is INT, with `/` truncating toward zero; any REAL operand makes
  the result REAL; `**` is REAL. `AND`, `OR`, `XOR`, `NOT` are logical on BOOL
  and bitwise on INT. Mixed BOOL and number is CF-523: write `x <> 0` or
  `if(b, 1, 0)`.
- An INT overflow, a division by zero, a `sqrt` or `ln` of a value out of its
  domain, or a REAL result that is not finite gives a BAD result with
  OVERFLOW; the tag keeps the computed time stamp.

Functions:

| Group | Functions |
| --- | --- |
| Selection | `if(c, a, b)`: `a` if `c`, else `b`; only the chosen branch is evaluated, and its quality counts with that of `c`. `limit(lo, x, hi)` |
| Maths | `abs`, `sqrt`, `exp`, `ln`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `round`, `trunc`, `floor`, `ceil`, `min(a, b, ...)`, `max(a, b, ...)` |
| Aggregates | `sum`, `avg`, `min`, `max`, `count_good`, each over one pattern string or over a list of two or more tag references: `sum("ps1.inv*.power")`, `max(a.p, b.p, c.p)` with tags. A single string argument makes `min` and `max` aggregates |
| Time | `moving_avg(x, T#5m)`, `rate(x, T#1m)` (change per second over the window), `totalise(x, T#1h)` (integrates `x`, a rate per given time, into a quantity). `x` must be a tag reference. Each call keeps its own state in the task; the window is 1 to 10,000 task cycles (CF-524) |
| Quality | `quality(x)`, `age(x)`, `is_good(x)` |

Quality and time of a result (reference 7.6): with `quality: worst` the
result has the worst quality of the inputs it used; with `quality: ignore_bad`
aggregates leave out BAD inputs, and the result is UNCERTAIN if any was left
out, BAD if all were. The source time is the newest source time among the
inputs used. A result sets no quality flag except OVERFLOW.

Patterns in aggregates are resolved at validation (section 2.5). A virtual tag
may use virtual tags; the validator orders them within each task and refuses
a cycle (CF-525), also through a pattern that matches the tag itself. A
virtual tag that uses a virtual tag of another task reads that tag's last
value, as any other input. An expression is at most 4,096 bytes and 32 levels
deep (CF-526). Expressions are compiled and type-checked at validation and
run under the instruction budget of their task (section 9.1).

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
| `interval` | duration, `10ms` upward | `periodic`, `both`: sample period. Required for them |
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
    authority: { default: web, local_panel: ps1.panel.local, local_blocks_auto: true }
    auto_resume: false
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `description` | text | none | |
| `authority.default` | `cli`, `web`, `api` | `web` | Holder of authority when nothing is retained (reference 8.3) |
| `authority.local_panel` | tag reference, BOOL | none | Local/remote switch input; true means local (reference 8.3) |
| `authority.local_blocks_auto` | bool | `false` | Local also blocks automatic commands |
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
    interlocks: [ps1.cb1.permissive]
    safe_state: hold
```

An output is the configured controllable point of reference 8.1. Keys:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | tag name | required | Base of the output's generated tags (section 12.5) |
| `area` | area reference | required | Reference 12.5 |
| `kind` | `single`, `double`, `pulse`, `latch`, `step`, `setpoint`, `trigger` | required | What a command means: a BOOL on/off (`single`), a position (`double`, value 1 off/open or 2 on/close), a pulse of `pulse_length`, a latch, a raise/lower step, a value (`setpoint`), an action with no off (`trigger`) |
| `on_only` | bool | `true` for `trigger`, else `false` | Only "on" is commanded; "off" does nothing on the device (resets, group selection) and is refused |
| `pulse_length` | duration | `1s` | `pulse` only |
| `target` | tag reference | required | A point with `access: rw` or `wo` (CF-702), not STRING. The tag that write requests name; its tag gets WRITABLE_BY_COMMAND (section 12.5) |
| `feedback` | tag reference, `{ on, off }` of two BOOL tags, or `{ tag, within, settle }` | required unless `no_feedback` | The state the device reports (reference 8.1): one tag, separate on and off tags, a double point, or a read-back with a tolerance and a settle time |
| `no_feedback` | `true` | | Explicit "no feedback" (reference 12.5). The write result is then the only confirmation, and the target's tag shows the last value written, unconfirmed |
| `feedback_timeout` | duration, `100ms` to `10m` | `10s` | Reference 8.2, step 10 |
| `sbo` | bool | `false` | Select before operate by `scada-cmd` (reference 8.2, step 6) |
| `select_timeout` | duration, `1s` to `60s` | `30s` | Reference 8.2, step 6; at most the write request lifetime of 60 s (tag database contract 14) |
| `native_select` | `at_select`, `at_operate` | `at_select` | For a target with a select on the device. Open: needs the product owner's ruling (X-1, X-7) |
| `reason_required`, `reauth` | bool | `false` | Reference 8.2, step 7; 13.2 |
| `limits` | `{ min, max, ranges, rate, release }` | none | Value validation (reference 8.2, step 4): `min`, `max`, or `ranges`, a list of `[lo, hi]` intervals (a union); `rate` (most change per second); `release`, a value outside the ranges that is still allowed, for setpoints that a release value turns off |
| `interlocks` | list of interlock references | required for kinds `single`, `double`, `pulse`, `latch`, `step`, `trigger` | Reference 8.2, step 5. Open: needs the product owner's ruling (X-13) |
| `refresh` | duration, `1s` to `1d` | none | The driver writes the accepted value again at this period, for devices whose setpoint lapses or that fall back when writes stop. Open: needs the product owner's ruling (X-5) |
| `write_on_change_only` | bool | `false` | `scada-control` posts a write only when the requested value changes; with `refresh` the refresh still runs |
| `safe_state` | `off`, `on`, `hold`, or `{ value }` | required (reference 12.5) | The state the output is driven to when a rule below says "safe" |
| `on_shutdown` | `hold`, `safe`, `release` | `hold` | Reference 14.4; `release` writes `limits.release` (X-9) |
| `on_control_loss` | `{ action: hold \| safe, after }` | `{ action: hold }` | When `scada-control` stops while the output is in AUTO (reference 14.4); `after` from `0s` to `10m` |
| `on_source_loss` | `hold`, `release`, `safe` | `hold` | For an output with `refresh`: when the requested value's source has been BAD or stale for `source_lost_after`, stop refreshing (`hold`), write `limits.release` once (`release`), or drive to `safe_state` (`safe`). Open: needs the product owner's ruling (X-9) |
| `source_lost_after` | duration, `0s` to `10m` | `0s` | |
| `task` | task reference | the task of the program that drives it, else `normal` | Owner of the AUTO request tag |
| `termination_timeout` | duration | reserved | For two-phase results of phase 9 (driver interface contract) |

Rules (each has its code in section 10):

- A target belongs to exactly one output (CF-703). The target's device is on
  a channel; the output's write requests go to that channel's command
  queues (section 12.3).
- `sbo` with `native_select`: whether a target has a select on the device is
  declared by its protocol at `configure` (driver interface contract). With
  `at_select`, `select_timeout` must not exceed the device's select window
  (CF-704); with `at_operate`, `scada-cmd` holds the operator's selection and
  the driver runs the device select and operate together at OPERATE.
- A target point that is `dangerous` makes the output dangerous: no program
  may drive it (CF-705), and `reason_required` and `reauth` are forced true.
  Further question 2 of the survey: the validator enforces it.
- A target point with `storage: nvm` cannot be driven by a program and cannot
  have `refresh` (CF-706).
- `refresh` needs a target that is `volatile`.
- A `trigger` or `on_only` output has no off value (CF-707).
- An output whose target is on a channel whose protocol has no write for the
  target's address shape is CF-708.

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

| Situation | Behaviour | Configured by |
| --- | --- | --- |
| Orderly shutdown, update | Hold, or safe state | `on_shutdown` |
| `scada-control` stops (AUTO) | Hold, or safe state after a time | `on_control_loss` |
| Generation change (data plane restart) | Hold, or safe state if the limit is exceeded | `on_shutdown`; `data_plane.generation_change_limit` (reference 5.5) |
| Refreshed setpoint, source BAD or stale | Stop refreshing, release, or safe state | `on_source_loss` |
| After a restart | Not driven until a full control cycle with valid inputs and the restore of retained modes and setpoints; AUTO resumes per area | `areas.<area>.auto_resume` |
| Local output of `scada-io` | Safe state on loss of control heartbeat, watchdog, shutdown (reference 6.5) | `safe_state` |
| The unit itself fails | The device's own communication-loss behaviour, set on the device (reference 14.4) | Not in this tree |

The behaviour itself is built in phase 5; phase 2 validates the keys.

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
realtime:
  cpus: [3]
```

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `tasks.<name>.period` | duration, `10ms` to `1h` | required | Reference 7.1 |
| `tasks.<name>.priority` | 1 to 99 | 50 | Thread priority; in the real-time tier, the `SCHED_FIFO` priority |
| `tasks.<name>.scan_group` | `<channel>.<poll group>` | none | Run once per scan of that poll group instead of on a timer (reference 6.2, 7.1). Reserved until the driver interface contract has synchronous scan groups |
| `tasks.<name>.rt_required` | bool | `false` | Reference 4.3, 7.5 |
| `tasks.<name>.overrun_alarm_after` | 1 to 1,000 | 3 | Consecutive overruns that raise a system alarm (reference 7.2) |
| `tasks.<name>.budget` | 1,000 to 100,000,000 | 1,000,000 | Instruction budget per cycle, for each program and the virtual tags of the task (reference 7.4) |
| `tasks.<name>.programs` | list of program references | `[]` | The ordered list of programs (reference 7.1) |
| `realtime.cpus` | list of CPU numbers | none | CPU isolation for real-time threads (reference 4.3) |

- Without `control/tasks.yaml`, the tasks `fast` (10 ms), `normal` (100 ms)
  and `slow` (1 s) exist (reference 7.1). A file replaces them all.
- Every program is listed in exactly one task (CF-901). A program file that
  gives `task` must name that task (CF-902).
- Timing rules of reference 7.5: a task whose period is shorter than the poll
  period of any input it reads is warning CW-903; a task marked
  `rt_required` that reads a tag of anything but `scada-io` or a synchronous
  scan group is CF-904.

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
statement   = ( assignment | if_stmt | case_stmt | for_stmt | call ) ";" ;
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
call        = identifier "(" [ identifier ":=" expression { "," identifier ":=" expression } ] ")" ;
```

`expression`, `tag_ref`, `literal` and `identifier` are those of section 5.6;
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
code; a rule that is removed keeps its code unused.

### 10.1 Files and syntax (CF-1xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-101 | A file at a path section 1 does not list, or a reserved file | `{path} is not a file of the configuration tree` |
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
| CF-203 | Wrong type | `{key} must be {type}` |
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
| CF-413 | `disable` or `overrides` of an unknown point | `point {name} does not exist in template {template}` |
| CF-414 | More than 65,536 points after expansion | `template {name} expands to {n} points, the limit is 65536` |
| CF-415 | A child changes a parameter's type | `parameter {name} cannot change its type` |
| CF-416 | Two points of one name in one level, or an instance point that exists | `point {name} is defined twice` |
| CF-417 | A point without `address` in a protocol template after expansion | `point {name} has no address` |
| CF-420 | `protocol` of the template does not match the channel's | `template {name} is for {protocol}, the channel is {channel_protocol}` |
| CF-421 | Base and protocol template mixed wrongly | `template {name}: {reason}` |

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

### 10.6 Drivers, devices and points (CF-6xx)

| Code | Rule | Message |
| --- | --- | --- |
| CF-601 | Two devices on one channel with the same address, or overlapping ranges, where the protocol forbids it (reference 12.5; Annex A.3) | `devices {a} and {b} overlap at {address}` |
| CF-602 | Two points of one device that overlap where the protocol forbids it | `points {a} and {b} overlap at {address}` |
| CF-603 | An address not valid for the protocol | `{name}: {reason}` |
| CF-604 | An unknown poll group | `poll group {name} does not exist on channel {channel}` |
| CF-605 | More devices on a channel than `limits.max_devices_per_channel` | `channel {name} has {n} devices, the limit is {limit}` |
| CW-606 | Pacing cannot serve a poll group. Open: needs the product owner's ruling (X-13): a warning in this draft | `channel {name}: poll group {group} needs about {needed} per cycle at the configured pacing, its period is {period}` |
| CW-607 | The slowest poll period or the demoted retry is longer than the device's idle time-out and `keepalive` is off | `device {name}: the connection may be closed between polls; set keepalive` |
| CF-610 | A `side_effect` point that is merged, retried, in two poll groups, or the target of `READ_NOW` | `point {name} has side effects: {reason}` |
| CF-611 | `max_age` below `update_period` | `point {name}: max_age is shorter than the device's update period` |
| CF-612 | A companion (exponent, quality, time) not read in the same request, or in another poll group | `point {name}: {companion} must be read with it` |
| CF-613 | A double point without a needed `states` map | `point {name}: states must be given` |
| CF-614 | A `wo` point in a poll group or with alarms or logging | `point {name} is write-only and cannot be read` |
| CF-615 | A device function not in the device's supported set (Annex A) | `point {name} needs function {fn}, which device {device} does not support` |
| CF-616 | An identity check on a point that is not read | `identity check {n} of {device}: point {point} is not read` |

### 10.7 Areas and outputs (CF-7xx)

| Code | Rule | Message |
| --- | --- | --- |
| CW-701 | An area used by nothing | `area {name} is not used` |
| CF-702 | An output target that is not a writable point | `output {name}: target {tag} is not a writable point` |
| CF-703 | A target in two outputs | `tag {tag} is the target of outputs {a} and {b}` |
| CF-704 | `select_timeout` longer than the device's select window | `output {name}: select_timeout {t} exceeds the device's select window {w}` |
| CF-705 | A dangerous output driven by a program | `output {name} is dangerous and cannot be driven by a program` |
| CF-706 | An `nvm` target driven by a program, or with `refresh` | `output {name} writes non-volatile memory and cannot be written cyclically` |
| CF-707 | An off value for an `on_only` output | `output {name} is on-only` |
| CF-708 | A target address the protocol cannot write | `output {name}: the protocol cannot write {address}` |
| CF-709 | No `feedback` and no `no_feedback` | `output {name} needs feedback or no_feedback: true` |
| CF-710 | No `safe_state` | `output {name} needs a safe_state` |
| CF-711 | A switching output without `interlocks`. Open: needs the product owner's ruling (X-13) | `output {name} needs interlocks (an empty list if none applies)` |
| CW-712 | A switching output with `interlocks: []` | `output {name} has no interlock` |
| CF-713 | A value (limit, safe value, release) outside the target's type | `output {name}: {key} does not fit the target's type` |
| CF-714 | `refresh` with a target whose device cannot refresh (no `volatile` storage), or `on_source_loss: release` without `limits.release` | `output {name}: {reason}` |
| CF-715 | A feedback tag of the wrong kind for the output's kind | `output {name}: feedback must be {kind}` |

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
| CF-1003 | An owner with more than 524,288 tags, or more than 1,023 owners (tag database contract 14) | `{reason}` |
| CF-1004 | A queue or owner name longer than 31 bytes (a protocol name over 12 bytes) | `queue name {name} is longer than 31 bytes` |
| CF-1006 | Two channels or two tasks with the same short name (section 12.3) | `{kind} {a} and {b} have the same short name {c}; rename one` |
| CF-1005 | A service user that does not exist on the unit, uid 0, or two processes with one uid | `process {name}: {reason}` |
| CF-1010 | `heartbeat_check_ms` above 400 with a `retain_now` tag (D-62, #96) | `heartbeat_check_ms must be at most 400 in a tag set with a retain_now tag` |
| CF-1011 | Any other refusal of `scada::tag::validate` | `tag set: {reason}` |

A refusal of the tag-set builder names the configuration line that caused it:
the tag's definition, the channel file of an owner, `system.yaml` for data
plane settings.

## 11. Versioning and migration

Decision D-80.

- The **tree format** is the integer `format` of `system.yaml`, 1 for this
  contract. A software version reads exactly one format; a tree of another
  format is CF-211.
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
  the service name (`docs/install.md`, one system user per service).
- **uid.** The tag set holds numeric uids (D-26). `scada-conf` looks up the
  uid of each service user on the unit when it builds the tag set. A user
  that does not exist, uid 0, or two processes with one uid is CF-1005 (the
  D-26 note for phase 2). The uid is in the canonical encoding, so
  `config_hash` depends on the unit as well as the tree (section 12.8).
  Tests give the lookup a fixed table of users, so that a tree's hash is
  pinned in a test (#101).
- The heartbeat timeout is `data_plane.processes.<process>.heartbeat_timeout_ms`,
  default 3,000 ms, and 500 ms for a REALTIME process (D-26).
- A driver process is not REALTIME in phase 2. A driver with a fast poll group
  in the real-time tier (reference 4.3) needs the synchronous scan groups of
  the driver interface contract and is added by a later minor version.
- `scada-conf` is not in the tag set: it does not attach.

### 12.3 Owners and queues

Decision D-75. Owner and queue names are at most 31 bytes (tag database
contract 1, 3), and the part of a queue name after its prefix is free text
there. They are therefore generated from the configured names with a **short
name**, so that channel and task names keep the 31-byte limit of an
identifier:

```
C(channel) = the first 10 hexadecimal digits (lower case) of SHA-256("channel:" + channel name)
T(task)    = the first 10 hexadecimal digits (lower case) of SHA-256("task:" + task name)
```

For example a channel `line1` gets `C(line1)`, ten digits such as
`3f9a1c22e0` (the value here is illustrative; the corpus of section 13 pins
real ones).

- A short name depends on its own name only. Adding, removing, reordering or
  renaming one channel or task changes the owner and queue names of that
  channel or task alone; every other name stays as it was.
- Two channels, or two tasks, whose short names are equal are CF-1006
  ("channels {a} and {b} have the same short name {c}; rename one"). With 40
  bits the chance for 1,000 channels is below one in a million.
- Protocol names (`modbus`, `io`, later ones) are at most 12 bytes, so
  `drv-<protocol>.<C>` is at most 27 bytes.
- `scada-conf` writes the map from every generated name to what it stands for
  (`drv-modbus.3f9a1c22e0` → channel `line1` of protocol `modbus`) to
  `/var/lib/scada/conf/tag-set-names.json` when it applies a tree, atomically
  and readable by group `scada`. `scadactl` shows both, for example
  `cmd.3f9a1c22e0.4b7d1e6a90 (channel line1, task normal)`; `scada-tagd` and
  the tag database know only the generated names, and their logs show those.

| Owner | Process | Writes |
| --- | --- | --- |
| `control.<T(task)>` | `control` | The task's virtual, internal and interlock tags, AUTO request tags, tunable parameter tags |
| `cmd` | `cmd` | Manual tags, MANUAL request tags, mode and status tags of outputs, authority tags of areas |
| `alarm` | `alarm` | Alarm state tags |
| `io` | `io` | The tags of every `scada-io` channel, with their system tags (one scan thread, reference 6.5) |
| `drv-<protocol>` | `drv-<protocol>` | The driver's process-level system tags `sys.proc.drv-<protocol>.*` (its main thread) |
| `drv-<protocol>.<C(channel)>` | `drv-<protocol>` | Every tag of the channel's devices, their `sys.dev.<device>.*` system tags and the channel's `sys.drv.<channel>.*` system tags (the channel's worker thread) |

- An owner without tags is not created; a process without owners is still in
  the process table.
- Change ring capacity of an owner: `data_plane.change_ring_capacity`, raised
  to the next power of two of at least twice the owner's tag count when that
  is larger (tag database contract 5, 8.3), at most 1,048,576 (CF-1003). The
  builder of the tag database does not raise it, so `scada-conf` states it for
  every owner (D-66 a, D-76).

Queues, each with the capacity of section 3. `<S>` is `cmd` for `scada-cmd`
and `<T(task)>` for a control task:

| Queue | Producer, consumer | Exists when |
| --- | --- | --- |
| `cmd.<C(channel)>.cmd`, `result.<C(channel)>.cmd` | `cmd`, the channel's driver process | The channel has a point (operator writes and `READ_NOW`) |
| `cmd.<C(channel)>.<T(task)>`, `result.<C(channel)>.<T(task)>` | `control`, the channel's driver process | A program of the task drives an output whose target is on the channel |
| `cmd.io.<S>`, `result.io.<S>` | `cmd` or `control`, and `io` | As above, for `scada-io` |
| `events.cmd` | `cmd`, `audit` | Always |
| `events.control.<T(task)>` | `control`, `audit` | A program of the task drives an output (no audit, no action applies to AUTO writes, tag database contract 9.3) |
| `events.alarm` | `alarm`, `audit` | Always |
| `events.drv-<protocol>` | the driver process, `audit` | From phase 5: the driver has an output whose `on_shutdown` or `on_source_loss` is not `hold`, since the safe or release write it makes on its own needs its event record first (driver interface contract) |

- The longest name is `result.<C>.<T>`: 7 + 10 + 1 + 10 = 28 bytes, within 31.
- Inside a driver, one command thread consumes every `cmd` queue of the
  process and hands requests to the channel workers (driver interface
  contract); the queue names do not depend on that.
- The consumer of the event queues is provisional: which service journals
  which record, and whether an AUTO write needs one record per request or
  per change of state, is decided with the command and audit services
  (phases 4 and 5, #83, D-66 g). A change of consumer is a tag set change and
  a minor version of this contract.

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
| `<output>.status` | `cmd` | INT64 | none | 0 IDLE, 1 SELECTED, 2 EXECUTING, 3 OK, 4 FAILED, 5 BLOCKED (reference 8.1) |
| `<output>.req_manual` | `cmd` | the target's type | RETAIN | The value last requested in MANUAL |
| `<output>.req_auto` | `control.<T(task)>` | the target's type | none | The value last requested by logic |
| `sys.area.<area>.authority` | `cmd` | INT64 | RETAIN, RETAIN_NOW | 1 LOCAL_PANEL, 2 CLI, 3 WEB, 4 API, 5 MASTER (phase 11) |
| Interlock `<name>` | `control.<T(task)>` | BOOL | none | Section 7.3 |
| `<tag>.alarm.<id>` | `alarm` | INT64 | RETAIN | 0 normal, 1 unacknowledged, 2 acknowledged, 3 returned unacknowledged, 4 latched unacknowledged, 5 latched acknowledged, 6 shelved, 7 suppressed, 8 out of service (reference 10.2) |
| `<tag>.alarm.<id>.shelved_until` | `alarm` | INT64 (ns UTC, 0 if not shelved) | RETAIN | |
| `ctl.<program>.<block>.<param>` | `control.<T(task)>` | The parameter's type | RETAIN | Tunable parameters (reference 7.3) |
| `sys.drv.<channel>.<name>`, `sys.dev.<device>.<name>` | `drv-<protocol>.<C(channel)>` (or `io`) | As the driver interface contract lists | none | Per channel and per device (reference 6.3) |
| `sys.proc.<process>.<name>` | `drv-<protocol>` (or `io`) | As the driver interface contract lists | none | Per driver process |

- For an expression alarm, `<tag>` is its `name`.
- The output's target is the point tag that write requests name; the
  feedback tag is a point tag of the device (reference 8.1).
- Driver system tags are listed, with their types, in the driver interface
  contract (section 10 of its draft 0.1); `scada-conf` builds them by its
  rule: one set per process, per channel, per poll group of a channel
  (`period.<group>`), and per device, with the identity tags only for a
  device with `identify`. None is retained. The system tags of the services themselves
  (task cycle times, the real-time tier, tagd) are added with those services
  (phases 4 to 7), each a minor version of this contract.

### 12.6 Agreement with tag database contract 1.0

- Names: tag names of at most 127 bytes of `[a-z0-9_.-]`, owner, process and
  queue names of at most 31 bytes, no owner name starting with `.`
  (contract 1). The grammar of section 2.1 is a subset.
- Queues: every `cmd.` queue has its `result.` queue (D-52); producer and
  consumer are different processes; capacities are powers of two up to
  2,147,483,648 (contract 5).
- Flags: `retain_now` only with `retain`; no STRING tag RETAIN or
  WRITABLE_BY_COMMAND (contract 5, 9.4, 12).
- `heartbeat_check_ms` at most 400 with a RETAIN_NOW tag (contract 12, D-62).
- Limits of contract 14: at most 1,000,000 tags, 1,023 owners, 524,288 tags
  per owner, 1,024 processes, 4,096 queues; ring capacity 1,024 to 1,048,576
  and at least twice the owner's tags.
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
canonical encoding of the tag set). It covers every file byte for byte, so a
change of a comment changes it too: it identifies a tree, not its meaning.
`scada-conf` stores it with each committed version.

## 13. Conformance tests

The configuration schemas are accepted against a corpus of trees in
`libs/scada-conf/tests/corpus/` (P2-03 to P2-06). Each case is a tree and an
expected result: for a valid tree, the expanded tag list and the tag set with
its `config_hash` for a fixed uid table; for an invalid tree, the list of
`(file, line, code)` it must give, in order. The tests run on x86-64 and
ARM64, and the parser runs under its fuzz target (#100).

1. Every example of this contract and of reference 12.3, assembled into one
   tree, is valid, and gives a pinned tag set and `config_hash`.
2. One tree per rule of section 10, each giving exactly its code at the
   expected line; a warning-only tree commits.
3. YAML subset: an anchor, an alias, a merge key, a tag, two documents, a
   duplicate key, `null`, `yes`, an octal number, a byte order mark, a NUL,
   each refused with CF-110 to CF-114 and the right column.
4. Expansion: a corpus of templates and instances (base and protocol
   template, `extends` to depth 4, parameters of every type, `when`,
   `disable`, `repeat` with one and two variables and irregular ranges,
   overrides of maps and lists, instance points) with the expected expanded
   tag sets; each step of the merge order of section 4.5 has a case where a
   different order would give a different result (#103).
5. An error inside an expanded instance names the instance and the template
   file and line (#103).
6. Name length: a prefix and point name of exactly 127 bytes is valid; 128 is
   CF-402 at the instance, naming the template line; the same for a generated
   tag (CF-403) (#119).
7. `heartbeat_check_ms` 400 with a `retain_now` tag is valid; 401 is CF-1010
   with its message (#96).
8. `retain_max_age` on an internal tag is valid; on an output mode tag
   (through an override), an authority tag, an alarm state tag and a counter
   it is CF-508 (#89).
9. The same tree in a different order of files and entries, and with files
   renamed (where names allow), gives the same `config_hash`; a tree that
   changes only an alarm limit, a poll period, a scale or a logging setting
   gives the same `config_hash`; a tree that adds one tag gives another
   (#101, tag database conformance test 20).
10. Every tag set the corpus builds passes `scada::tag::validate`; a tree
    that would break a limit of tag database contract 14 is refused with its
    file and line.
11. Expressions: every operator and function with BOOL, INT, REAL and TIME,
    overflow and division by zero giving BAD with OVERFLOW, `worst` and
    `ignore_bad`, patterns, cycles, ambiguous attributes; the same results on
    both architectures.
12. A tree of format 0 (a test format) is migrated to format 1 by a test
    migration, keeping the comments of lines it does not change.
13. Short names: the corpus pins `C()` and `T()` of a few names; adding,
    removing, reordering and renaming one channel or task leaves every
    other owner and queue name unchanged; two names given equal short names
    by a test hook are CF-1006; the name map file lists every generated name.
14. `config_id` is the same for one tree on two uid tables, where
    `config_hash` differs; it changes with any byte of any file.
15. Modbus (Annex A): addresses of every type and order, overlaps between
    devices with shared unit ids (refused and allowed cases), point-level
    unit ids, functions outside the supported set.

## 14. Open rulings

Each place marked "open" above is drafted with the option this contract
proposes. The product owner rules on each; until then the contract stays a
draft. The pull request of version 0.1 lists the options of each.

| Ruling | Subject | Drafted as | Where |
| --- | --- | --- | --- |
| X-1 | A select on the device for protocols where reference 6.4 assumes none | Per device and point, declared by the protocol | 7.2 |
| X-2 | Channels opened by the device | `role` key, only `connect` accepted | 4.6 |
| X-3 | One device on two connections (main and backup) | Not in phase 2; room in the protocol's channel block | Annex A |
| X-4 | Several devices on one device address, one device on several | Allowed with disjoint ranges; point-level unit id | A.3 |
| X-5 | Refreshing a setpoint without a new operator action | `refresh`, under the command's one audit record | 7.2 |
| X-7 | Device select windows shorter than the gateway's select | `native_select: at_select` (validated) or `at_operate` | 7.2 |
| X-8 | Several protocols of one device type | One protocol per template, protocol-free base | 4.1 |
| X-9 | Release or safe value written to a remote setpoint | `on_source_loss`, `on_shutdown` | 7.2, 7.4 |
| X-12 | Who sets device clocks | `time_sync: none` only | 4.6 |
| X-13 | Errors or warnings for pacing and for outputs without interlocks | Pacing a warning; missing `interlocks` an error, empty list a warning | 10.6, 10.7 |
| X-14 | Feedback with four states | Double-point feedback accepted | 7.2 |
| X-15 | Point lists from device files | Generated templates are ordinary templates; importers later | 4.2 |
| X-16 | Settings edits and setting-group switching | Group switching is an `on_only` output; settings edits out of scope | 7.2 |
| X-17 | Modbus function codes | A minimum set with a per-device policy | A.1 |

X-6, X-10 and X-11 belong to the driver interface contract; this contract
refers to its answer.

## 15. Change log

| Version | Date | Change |
| --- | --- | --- |
| 0.1 | 2026-11-22 | First draft (issue #98, P2-01), from the outline in `README.md`, reference 1.1 and the device survey of 2026-11-22 (not in the repository). Decisions D-69 to D-80. |

## Annex A. The Modbus protocol schema

The protocol keys of `drivers/modbus/*.yaml` and of templates with
`protocol: modbus`. Section 4.1 says how they combine with the framework
keys. The driver (#105) checks them again at `configure` (driver interface
contract).

### A.1 Channel and device keys

| Key | Object | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| `type` | channel | `rtu`, `tcp`, `rtu_over_tcp` | required | `rtu_over_tcp`: RTU frames with their CRC over a TCP connection (serial servers, some meters' ports). `ascii` and `udp` are reserved |
| `port` | channel, `rtu` | device path | required | `/dev/tty...`; the driver needs access to it (reference 4.2) |
| `baud` | channel, `rtu` | 300 to 4,000,000 | 19,200 | Any rate the port supports |
| `data_bits` | channel, `rtu` | 7, 8 | 8 | |
| `parity` | channel, `rtu` | `none`, `even`, `odd` | `even` | |
| `stop_bits` | channel, `rtu` | 1, 2 | 1 | |
| `turnaround` | channel, `rtu` | duration, `0ms` to `1s` | `0ms` | Extra silence after a broadcast or before the next request |
| `host` | channel, `tcp`, `rtu_over_tcp` | host name or address | required | |
| `port` | channel, `tcp`, `rtu_over_tcp` | 1 to 65,535 | 502 | Devices use other ports too |
| `connect_timeout` | channel, `tcp`, `rtu_over_tcp` | duration, `100ms` to `60s` | `5s` | |
| `unit_id` | device | 0 to 255 on `tcp`; 1 to 247 on `rtu` and `rtu_over_tcp` | required | Unit 0 on TCP is an ordinary unit (some devices answer only on it); broadcast is not used |
| `functions` | device | list from 1, 2, 3, 4, 5, 6, 15, 16 | all eight | Functions the device supports. The driver uses no other (CF-615), and substitutes where it can: 16 for 6, 3 for 4 only where `read.input_as_holding` says the tables are the same. Open: needs the product owner's ruling (X-17); this reads reference 6.4 as the minimum set |
| `address_base` | template, device | 0, 1 | 0 | `register` values are as the device documents them: with base 1, register 1 is protocol address 0. No other arithmetic is applied: a documented "40001" is written as the register the document means with its base, never as an offset of 40,000 |
| `read` | template, device | read rules (A.4) | as A.4 | |

Main and backup connections for one device (X-3) are not in this version;
room is left as a reserved channel key `connections`.

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
| `register` | integer or expression | required unless `parts` | The address in the table, with `address_base`; 0 to 65,535 after the base (CF-603) |
| `type` | below | `bool` for coils and discrete inputs, `uint16` otherwise | Raw type |
| `byte_order`, `word_order` | `big`, `little` | `big`, `big` | Byte order within a register, register order within a multi-register value; template default, overridable per instance (`defaults`) and per point |
| `bit` | 0 to 15 | none | One bit of a register: a BOOL |
| `field` | `[lo, hi]` | none | A bit field of a register or of a multi-register value, extracted and shifted down |
| `bit_order` | `lsb0`, `msb0` | `lsb0` | Bit 0 is the least or the most significant |
| `byte` | `high`, `low` | none | For `int8`, `uint8`: which byte of the register |
| `parts` | list of `{ register }`, most significant first | none | A value from registers that are not adjacent, in this order |
| `length` | 1 to 256 bytes | required for `string`, `bytes` | |
| `encoding` | `ascii`, `utf8`, `latin1` | `ascii` | For `string`; `padding` (`nul`, `space`) is stripped |
| `count` | 2 to 4 | required for `mod10k` | Registers of a modulo-10,000 value |
| `width`, `resolution` | 32 or 64; `1s`, `1ms`, or `<n>ns` | required for `epoch` | Integer time since 1970-01-01 in steps of `resolution`; converted to INT64 ns UTC, with the device's `device_time` |
| `fields` | list of `year`, `month`, `day`, `hour`, `minute`, `second`, `ms` | required for `datetime` | One field per register, from `register` on, in this order; converted with `device_time` |
| `unit_id` | as the device's | the device's | The unit id of this point, for one device that answers on several unit ids (open: needs the product owner's ruling (X-4)) |
| `write` | write encoding (A.5) | plain write of the value | For points with `access: rw` or `wo` |

Raw types: `bool`, `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`,
`int64`, `uint64`, `float32`, `float64`, `string`, `bytes` (as `string`
without decoding, shown as hex), `mod10k`, `epoch`, `datetime`. A 24-bit value
in a 32-bit pair is `uint32` with `field: [0, 23]`. A value of more than one
register may start at any register, odd or even. A string tag holds at most
250 bytes (tag database contract 14): a longer device string is cut to 250
bytes and written with OVERFLOW.

Double points (`kind: double_point`, section 5.3) take one of these address
shapes:

| Shape | Example | `states` |
| --- | --- | --- |
| Two bits | `{ on: { table: holding, register: 60, bit: 4 }, off: { table: holding, register: 60, bit: 5 } }` | Keys are the on bit then the off bit. Default `{ "00": 0, "01": 1, "10": 2, "11": 3 }` |
| Two coils or discrete inputs | `{ on: { table: discrete, register: 20 }, off: { table: discrete, register: 21 } }` | As above |
| A 2-bit field | `{ table: holding, register: 61, field: [0, 1] }` | Required: raw 0 to 3 to the four codes |
| An integer | `{ table: holding, register: 62, type: uint16 }` | Required |

### A.3 Device addresses on a channel

Reference 12.5 says "no overlapping or duplicate device addresses on a
channel". For Modbus this contract reads a device's address as the set of
`(unit id, table, register)` its points read or write. Open: needs the product
owner's ruling (X-4).

- Two devices on one channel may share a unit id when their sets are
  disjoint: slices of one gateway unit, each a device of its own. Overlap
  is CF-601. Such devices share demotion in fact (one unit answers for both),
  and each is demoted on its own failures.
- One device may answer on several unit ids: its default `unit_id` and the
  `unit_id` of some point addresses.
- Within one device, two points may not use the same register of one table
  for different values (CF-602), except that bits and bit fields of one
  register, and a register and its bit points, may share it.
- The same data in two tables (a register readable as bits and as a register)
  is a template choice; the driver never merges across tables.

### A.4 Read rules

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `max_registers` | 1 to 125 | 125 | Registers per read |
| `max_bits` | 1 to 2,000 | 2,000 | Bits per read |
| `max_gap` | 0 to 124 | 0 | Unrequested registers a block read may bridge |
| `gap_policy` | `illegal`, `reads_zero`, `reads_sentinel` | `illegal` | What the device does with an unmapped register: an exception, zeros, or a marker. With `illegal`, `max_gap` must be 0 (CF-208); bridged registers are never decoded |
| `no_read` | list of `[first, last]` | none | Registers never read, also not inside a block |
| `atomic` | list of `[first, last]` | none | Registers always read in one request, and never split; a multi-register value is always atomic |
| `exclusive` | list of `[first, last]` | none | Registers read alone, in their own request, never merged (for record pointers and registers that change state when read) |
| `max_write_registers` | 1 to 123 | 123 | Registers per write |
| `input_as_holding` | bool | `false` | The input table is the holding table (for function substitution) |

A point with `side_effect` is in an `exclusive` range by itself. The block
plan the driver makes from these rules is its own (driver interface
contract), and shown in its diagnostics.

### A.5 Write encodings

`write` on a point with `access: rw` or `wo`, or the address shapes of an
output target:

| Key | Meaning |
| --- | --- |
| `function` | `5`, `6`, `15`, `16` or `auto` (default: 5 for one coil, 6 for one register, 16 for more, within `functions`) |
| `value` | A fixed raw value written whatever is commanded, for "write anything to act" registers |
| `values` | A map from the commanded value to the raw value written: `{ true: 0, false: 1 }` for inverted polarity, `{ 1: 0xFF00 }` |
| `on`, `off` | Two addresses (each `{ table, register, value }`), for commands with separate registers or coils for on and off |
| `mask` | For a whole-word command register: the bits this point sets; the other bits are written 0. Never read-modify-write |
| `type` | The raw type written, when it differs from the type read |
| `quantity` | Registers the device expects in the request, when it is more than the value's own |

Device-side select (roles select, operate, cancel on separate coils) and
commands with a password register are reserved: they need the ruling on X-1
and on sessions with secrets (further question 1 of the survey).
