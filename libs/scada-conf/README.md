# libscada-conf

Validation of the configuration tree, as the configuration schemas contract
defines it (`docs/contracts/config-schemas.md`, issue #100, P2-03). It is
built in slices; this README lists what each part does now.

## What it does

- Collects diagnostics (errors `CF-<n>`, warnings `CW-<n>`) with their file,
  line and column, in the order of contract section 1.4, keeping the first
  1,000 of each and a count (section 1.3). Every message, path and note is
  made printable when it is added (D-110): text of a file that holds a line
  break, a terminal escape or a character that reorders text is shown as
  visible escapes (`\n`, `\x1B`, `\u202E`), and cut at 1,024 bytes. A key or
  value quoted in a message has its `\` and `"` escaped (`quote_text`).
- Stage 1 of validation (section 1.4): reads a tree without following links
  and without opening anything outside its root, and refuses paths the
  contract does not list (CF-101), a missing `system.yaml` (CF-103) and files
  or trees over their size limits (CF-113). The scan expects a tree that
  nobody writes while it runs, as `scada-conf` validates a staging copy: an
  entry that vanishes or changes during the scan is an I/O error of the scan,
  not a diagnostic.
- Stage 2: parses every YAML file of the tree with the project's own parser
  of the YAML subset of section 1.1 (below) into a `Document` (below) that
  keeps each scalar's text, its quoting style, and its file, line and column.
  Structured text is not available in this version: every `control/*.st` file
  is CF-920 at 1:1, "structured text is not available in this version"
  (contract 9.3).

- Stage 3: types every scalar by the schema of its key (contract 1.2),
  keeping the types in a `ScalarTypes` table beside each Document (one byte
  per node, `include/scada/conf/types.h`, D-111), and
  checks each file against its JSON Schema file under `schemas/` (Annex C),
  with CF-2xx codes; also CF-102 (a file name and its name key), CF-111 for
  keys equal once typed, CF-211 (`format`) and the conditional
  `time.sync.servers` of section 3, the keys an object requires or refuses
  by the value of another key (a channel's type, a tag's, an alarm's;
  `rules.h`, D-108), and CF-517 for an explicit integer `value_type` within
  one point object. Validation checks one document node at most 21 times with the
  built-in schemas; a schema set that could pass 64 (nested `oneOf` or
  `allOf`) does not compile (D-107). Stage 3 checks every file that parsed,
  also when another did not. Measured (release build, x86-64, the lead's
  delta check of #132): `validate_tree` on 983,079 nodes in 1.49 s, on
  3.93 million nodes in 5.89 s, about 30 % slower than before the rules of
  stage 3; stage 5 will add a walk of its own.

Every file type has its schema: `system`, `areas`, `enums`, `logging`,
`tasks`, `tags`, `outputs`, `alarms`, `program`, and for templates and
channels the framework keys (`template`, `channel`) composed with their
protocol's schema (`modbus`, Annex A; `io`, Annex B), with the shared
`common` and `point` definitions. A channel file is checked against the
schema of its protocol directory, a template against that of its
`protocol` key; a template whose protocol this version does not know is
checked against the framework keys alone. Maps that merging completes (a
point in `defaults` or `overrides`, an address, every map of a point) are
not refused at stage 3 for a key they lack; each takes `replace: true`.

Expressions (contract 5.6) are parsed at stage 3 by `parse_expression`
(`include/scada/conf/expression.h`): run-time expressions (`expr`,
`suppress_when`, interlock `condition`) and configuration expressions
(`when`, addresses and scaling, `repeat` ranges), each with its own syntax
codes (CF-520, CF-526, CF-528, CF-529).

Stage 4 runs when stages 1 to 3 found no error: `expand_tree`
(`include/scada/conf/expand.h`, D-114) expands every device instance in the
merge order of contract 4.5 into one `Document` of the expanded channels,
devices and points, each value with the file and line it came from. An
error that a template's line causes is at the instance, with a note
`from template <file>:<line>:<column>`; a template's own faults are at the
template, once, whether or not an instance uses it. `validate_and_expand` gives the
report and the expanded tree, which the expanded files of contract 13.3 are
written from (#102). Expansion is linear in the points but for a lookup by
name: an instance of 65,536 points, made by one `repeat`, and the refusal of
one of 65,537 took 736 ms together (release build, x86-64,
`Expand.ThePointLimit`).

The expanded tree is held to the limits of a tree (contract 1.3, D-114):
4,000,000 nodes and 256 MiB of text (CF-113), and `limits.max_tags`
(CF-1001), each on a running total; passing one stops the expansion. The
nodes and tags are checked after every device, the text after every
point. Each device is written into the document as soon as it is
finished and then dropped. What an instance makes before step 4, and the
defaults merged under its points before step 5, count toward the node
limit, so memory is bounded by the limit, not by the tree's instances.
Every point made, written or generated, and every `repeat` value evaluated
count toward a work budget of 4,000,000, so instances times template
points is at most 4,000,000, whatever `only`, `disable` or `when` keep.
An expression is parsed once for all its evaluations and typed again only
when a name it reads changes type. Expressions have a budget of their
own, 128,000,000 nodes (32 times the node limit): every evaluation counts
its nodes, a reference to a parameter also the size of its value, and
every parse and typing its nodes. The diagnostics kept are at
most 2,000, past which an error is counted only.

The bound, measured on the worst accepted trees (release build, x86-64,
the whole of `validate_and_expand`):
- memory: a peak of 858 MB (838 MiB) for one instance of 62,000 points,
  each with an alarm `expr` of 4,056 bytes and 36 `invalid_values` (252
  MiB of text, 3,844,027 nodes); 670 MB for 5 `repeat` blocks of 65,536
  points whose `when` is false; 641 MB for one instance of 3,997,723
  nodes;
- time: about 12.8 s for the worst tree of expressions within their
  budget (additions and uncached parses take about 96 ns a node), 10.7 s
  for 61 instances of a template of 65,536 written points that each keep
  one (`only`), just under the work budget.

A tree past a limit is refused within that: a `when` of 400 terms over
65,536 values in 30 instances in 5.2 s (it took 729 s when every
evaluation parsed its text again), 100 instances of a 65,536-point
`repeat` at the node limit, 20,000 instances whose expressions fail at
18 MB, as 100 errors that each name the other instances.

Stage 5 runs when stage 4 found no error. On each merged point of the
expanded tree (`src/merged_points.cpp`, D-115) it checks the keys that
merging had to complete (CF-202), a value type that cannot hold its raw
type (CF-504, the pairs that contract 4.7 decides), the range of an integer
scaling (CF-516) and an integer value type with a scaling that is not
integer (CF-517, for every protocol). Each value computed from a
configuration expression (`register: "r * 65536"`, the ends of
`read.ranges`, a `health` rule's field) is checked by
`SchemaSet::check_value` against its key's rules in the protocol's schema,
as stage 3 checks a value written there (CF-204 and the key's other rules),
and a computed bit field as a whole (lo <= hi); a point with a wrong
computed value gets no further check. One cause at many instances is one
error that names the others, as in stage 4. About 2 µs a computed value:
393,216 take 0.55 s (D-115). It also checks what needs no point tags (`check_references`,
`include/scada/conf/references.h`, D-113):
- names that must exist (CF-310): templates and `extends`, a template's
  protocol, areas, logging policies and aggregate tiers, enumerations (of
  the template's chain or `enums.yaml`), tasks, programs, interlocks,
  outputs, alarm priorities and classes;
- names given twice: channels, devices and interlocks (CF-303), and the tags
  of tag files and interlocks (CF-301, CF-302);
- the Modbus channel rules of Annex A.1 (CF-622);
- the types of a template's configuration expressions, over the names of
  its chain upward (CF-410; CF-523 for the type of a `for` value), and a
  name in an expression of a device without template (CF-410). A name the
  chain upward lacks may come from a child, and is checked at the instance
  (#103).

Stage 5 also builds the tags of the expanded tree by name, with their value
types (`src/tag_names.cpp`, D-116): point and bit tags, the tags of tag files,
interlocks, and the generated tags of section 12.5. The tags of a device
(`sys.dev.*`) and a channel (`sys.drv.*`) are found from their names and the
signal tables of driver interface contract 11.1, not stored one by one. It
gives CF-301 for a user's name in `sys` (a tag, a device, an output, an
expression alarm), CF-302 for a name given twice, generated names included,
CF-403 for a generated name over 127 bytes and CF-1001 when the tags,
generated tags included, pass `limits.max_tags`. It checks every reference
to a tag outside an expression (outputs, the local panel, alarms wherever
they stand, programs: CF-310, and CF-312 for a `limit` or `setpoint` that
is not a number), every point a device's rules name, `applies_to` included
(CF-310, CF-311), and each point's poll group, given or the default
`normal` (CF-604). Errors on the expanded tree are grouped as in stage 4.
Owner names are #101's and are not used.

Stage 5 then checks every run-time expression against those tags
(`src/runtime_expressions.cpp`, D-117): virtual tags, alarm expressions,
`suppress_when` and interlock conditions. Virtual tags are typed first, in
the order their inputs need, and their derived types enter the tags before
any later check reads them. It gives CF-310 for a name that is no tag,
CF-311 for a pattern that matches none, CF-521 for an ambiguous `.quality`
or `.age`, CF-522 for a STRING tag, CF-523 for a type that does not fit or
a result that must be BOOL, CF-524 for a time function's window out of its
task's range, CF-525 for virtual tags that form a cycle and CF-527 for a
given `value_type` that is not the derived one.

Not in phase 2: structured text is not parsed, and the block library
declares no block type until phase 6, so a tree with control programs does
not validate before phase 6.

## The codes of contract section 10

Where each error code of section 10 is given, or which issue holds it (the
ruling on #100: #100 closes without the codes of stages 6 and 7, of the
protocols' own refusals and of what is not in phase 2). Kept current by each
slice of #100.

| Code | Where |
| --- | --- |
| CF-101 | `src/tree_scan.cpp` |
| CF-102 | `src/validate.cpp` |
| CF-103 | `src/tree_scan.cpp` |
| CF-110 | `src/yaml_parser.cpp` |
| CF-111 | `src/schema.cpp`, `src/yaml_parser.cpp` |
| CF-112 | `src/yaml_parser.cpp` |
| CF-113 | `src/expand.cpp`, `src/tree_scan.cpp`, `src/yaml_parser.cpp` |
| CF-114 | `src/yaml_parser.cpp` |
| CF-201 | `src/rules.cpp`, `src/schema.cpp` |
| CF-202 | `src/expand.cpp`, `src/merged_points.cpp`, `src/rules.cpp`, `src/schema.cpp`, `src/validate.cpp` |
| CF-203 | `src/rules.cpp`, `src/schema.cpp` |
| CF-204 | `src/merged_points.cpp`, `src/rules.cpp`, `src/schema.cpp` |
| CF-205 | `src/rules.cpp`, `src/schema.cpp` |
| CF-206 | `src/schema.cpp` |
| CF-207 | `src/schema.cpp` |
| CF-208 | `src/schema.cpp` |
| CF-210 | `src/schema.cpp` |
| CF-211 | `src/validate.cpp` |
| CF-212 | `src/expand.cpp`, `src/rules.cpp`, `src/schema.cpp` |
| CF-213 | `src/schema.cpp` |
| CF-214 | `src/schema.cpp` |
| CF-301 | `src/tag_names.cpp` |
| CF-302 | `src/tag_names.cpp` |
| CF-303 | `src/references.cpp` |
| CF-310 | `src/expression_types.cpp`, `src/merged_points.cpp`, `src/references.cpp`, `src/tag_names.cpp` |
| CF-311 | `src/tag_names.cpp` (`applies_to`); `src/expression_types.cpp` (expressions) |
| CF-312 | `src/tag_names.cpp` |
| CF-401 | `src/expand.cpp` |
| CF-402 | `src/expand.cpp` |
| CF-403 | `src/expand.cpp`; `src/tag_names.cpp` (the names of 12.5) |
| CF-410 | `src/config_eval.cpp`, `src/expand.cpp`, `src/references.cpp` |
| CF-411 | `src/expand.cpp` |
| CF-412 | `src/expand.cpp` |
| CF-413 | `src/expand.cpp` |
| CF-414 | `src/expand.cpp` |
| CF-415 | `src/expand.cpp` |
| CF-416 | `src/expand.cpp` |
| CF-417 | `src/expand.cpp` |
| CF-419 | `src/expand.cpp` |
| CF-420 | `src/expand.cpp` |
| CF-421 | `src/expand.cpp` |
| CF-422 | `src/expand.cpp` |
| CF-504 | `src/merged_points.cpp` |
| CF-505 | slice 7c of #100 |
| CF-506 | slice 7c of #100 |
| CF-507 | slice 7c of #100 |
| CF-508 | slice 7c of #100 |
| CF-509 | slice 7c of #100 |
| CF-520 | `src/expression.cpp`, `src/schema.cpp` |
| CF-521 | `src/expression_types.cpp` |
| CF-522 | `src/expression_types.cpp` |
| CF-523 | `src/config_eval.cpp`, `src/expand.cpp`, `src/expression_types.cpp`, `src/references.cpp`, `src/runtime_expressions.cpp` |
| CF-524 | `src/expression_types.cpp` |
| CF-525 | `src/runtime_expressions.cpp` |
| CF-526 | `src/expression.cpp` |
| CF-527 | `src/runtime_expressions.cpp` |
| CF-528 | `src/expression.cpp` |
| CF-529 | `src/expression.cpp` |
| CF-516 | `src/merged_points.cpp` |
| CF-517 | `src/merged_points.cpp`, `src/rules.cpp` |
| CF-601 | slice 7d of #100 |
| CF-602 | slice 7d of #100 |
| CF-603 | slice 7d of #100 |
| CF-604 | `src/tag_names.cpp` |
| CF-605 | slice 7d of #100 |
| CF-610 | slice 7d of #100 |
| CF-611 | slice 7d of #100 |
| CF-612 | slice 7d of #100 |
| CF-613 | slice 7d of #100 |
| CF-614 | slice 7d of #100 |
| CF-615 | slice 7d of #100 |
| CF-616 | slice 7d of #100 |
| CF-618 | slice 7d of #100 |
| CF-619 | slice 7d of #100 |
| CF-620 | slice 7d of #100 |
| CF-622 | `src/references.cpp` |
| CF-640 | slice 7d of #100 |
| CF-650 to CF-699 | drivers, through #104 and #102 (protocol refusals of section 13.1) |
| CF-702 | slice 7e of #100 |
| CF-703 | slice 7e of #100 |
| CF-704 | slice 7e of #100 |
| CF-705 | slice 7e of #100 |
| CF-706 | slice 7e of #100 |
| CF-707 | slice 7e of #100 |
| CF-708 | slice 7e of #100 |
| CF-709 | slice 7e of #100 |
| CF-710 | slice 7e of #100 |
| CF-711 | slice 7e of #100 |
| CF-713 | slice 7e of #100 |
| CF-714 | slice 7e of #100 |
| CF-715 | slice 7e of #100 |
| CF-716 | slice 7e of #100 |
| CF-717 | slice 7e of #100 |
| CF-718 | slice 7e of #100 |
| CF-719 | slice 7e of #100 |
| CF-720 | slice 7e of #100 |
| CF-723 | slice 7e of #100 |
| CF-722 | slice 7e of #100 |
| CF-801 | slice 7c of #100 |
| CF-802 | slice 7c of #100 |
| CF-803 | slice 7c of #100 |
| CF-811 | slice 7c of #100 |
| CF-812 | slice 7c of #100 |
| CF-813 | slice 7c of #100 |
| CF-814 | slice 7c of #100 |
| CF-815 | slice 7c of #100 |
| CF-901 | slice 7e of #100 |
| CF-902 | slice 7e of #100 |
| CF-904 | slice 7e of #100 |
| CF-905 | slice 7e of #100 |
| CF-906 | slice 7e of #100 |
| CF-910 | not in phase 2 (block library and structured text) |
| CF-911 | not in phase 2 (block library and structured text) |
| CF-912 | not in phase 2 (block library and structured text) |
| CF-913 | not in phase 2 (block library and structured text) |
| CF-914 | not in phase 2 (block library and structured text) |
| CF-915 | not in phase 2 (block library and structured text) |
| CF-916 | not in phase 2 (block library and structured text) |
| CF-920 | `src/validate.cpp` |
| CF-921 | not in phase 2 (block library and structured text) |
| CF-922 | not in phase 2 (block library and structured text) |
| CF-1001 | `src/expand.cpp` (point and bit tags); `src/tag_names.cpp` (all tags) |
| CF-1002 | #101 and #102 (stages 6 and 7) |
| CF-1003 | #101 and #102 (stages 6 and 7) |
| CF-1004 | #101 and #102 (stages 6 and 7) |
| CF-1006 | #101 and #102 (stages 6 and 7) |
| CF-1007 | #101 and #102 (stages 6 and 7) |
| CF-1008 | #101 and #102 (stages 6 and 7) |
| CF-1009 | #101 and #102 (stages 6 and 7) |
| CF-1005 | #101 and #102 (stages 6 and 7) |
| CF-1010 | #101 and #102 (stages 6 and 7) |
| CF-1011 | #101 and #102 (stages 6 and 7) |
| CF-1012 | #104 and #102 (protocol refusals of section 13.1) |
| CF-1016 | #101 and #102 (stages 6 and 7) |
| CF-1014 | #101 and #102 (stages 6 and 7) |
| CF-651 | slice 7d of #100 |
| CF-652 | slice 7d of #100 |
| CF-653 | slice 7d of #100 |
| CF-662 | slice 7d of #100 |

## The YAML subset parser

`parse_yaml` (`include/scada/conf/yaml.h`) is a **subset parser, not a YAML
parser** (D-106). It accepts exactly the subset of contract section 1.1 and
refuses everything else. The first refusal ends the parse of that file and is
its one error, with its line and column (columns count characters).

Accepted: one document of block and flow mappings and sequences; keys that
are plain or quoted scalars on one line; plain, single-quoted and
double-quoted scalars, also over several lines; literal (`|`) and folded
(`>`) block scalars with chomping and indentation indicators; comments; LF
or CRLF line ends. A file of comments only is an empty map.

Refused:

| Construct | Code |
| --- | --- |
| Anchors (`&a`), aliases (`*a`), tags (`!x`, `!!str`), merge keys (`<<`), complex keys (`? `), a collection as a key, a key over several lines | CF-110 |
| Document markers (`---`, `...`), directives (`%YAML`), several documents | CF-110 |
| A byte order mark | CF-110 |
| A mapping inside a flow sequence without braces (`[a: b]`), an empty flow entry (`[a, , b]`) | CF-110 |
| A line inside a flow collection not indented more than its key; a closing bracket there has its own message: indent the bracket, or write the collection as a block | CF-110 |
| The reserved indicators `@` and `` ` ``, an unknown escape, an escape giving U+0000 or a surrogate | CF-110 |
| Any other text outside the grammar: bad indentation, an unterminated quote, an unclosed bracket, text after a value | CF-110 |
| Two keys of one map with the same text (`a` and `'a'`) | CF-111 |
| `null`, `Null`, `NULL`, `~` as plain values, an empty value, an empty sequence item | CF-112 |
| A file over 8 MiB, more than 1,000,000 nodes in a file or 4,000,000 in the tree, more than 32 nested collections, more than 65,536 entries in a collection, a scalar over 64 KiB, a key over 64 bytes | CF-113 |
| Invalid UTF-8 (overlong forms, surrogates, above U+10FFFF, cut short), NUL, a carriage return without a line feed, other C0 control characters, DEL, C1 control characters, U+FFFE, U+FFFF | CF-114 |
| A tab anywhere but in a comment, a quoted scalar or a block scalar's content; indentation is counted in spaces | CF-114 |

The scalar limit holds for the value as it will be, whatever the input: a
scalar's lines are counted before they are stored, trailing white space of a
line is not counted (it is dropped), and the empty lines of a block scalar
are kept as a count, so that `a: |` followed by 8 MiB of empty lines costs no
memory.

Scalars are not typed here: `yes`, `010` and `0x10` are text, and their type
is decided by the schema of their key (contract 1.2, stage 3). Keys that are
equal only once typed (`16` and `0x10`) are found by stage 3 (CF-111). Template expansion (stage 4) is
#103, the tag set (stage 6) #101, the protocols' check mode (stage 7) #102.

## The node type

`include/scada/conf/node.h`. A `Document` holds the nodes of one or more
files; it is immutable, and only a `DocumentBuilder` makes one (the parser,
and later the loader of the expanded file, section 13.3). Reading a const
`Document` from several threads at once is safe: no accessor changes
anything, and there are no lazy caches.

- Nodes are `NodeId`s. `kind`, `style`, `text`, `position` (file index,
  line, column) and `location` (with the file's name). The type a schema
  gives a scalar is kept by stage 3 beside the Document, by NodeId. `entries` of a map and `items` of a sequence are
  spans, in the order of the file; on another kind of node they are empty.
  No accessor throws; an id of another document ends the process, as a
  programmer error that input cannot cause. A moved-from Document must not
  be read.
- `find(map, key)` is a binary search over the map's keys sorted once at
  build time: O(log n) in the map's entries.
- Memory: 20 bytes per node, 12 bytes per map entry, 4 bytes per sequence
  item, and the bytes of the scalars' text. A map entry is two nodes, so the
  structure costs at most 26 bytes per node: 26 MB at the node limit of a
  file (1,000,000 nodes), 104 MB at the tree's (4,000,000). The text is at
  most the file's bytes, and up to 1.5 times them where escapes such as `\P`
  give longer characters. Stage 3 and stage 5 keep every file's `Document`,
  and its table of types (1 byte per node), until validation ends; each
  file's bytes are freed as soon as it has parsed, so the tree is not held
  twice.
- Measured peak (x86-64, the whole of `validate_tree`): 541 MB for a tree at
  every limit at once: 256 MiB in 37 files, 3,974,081 nodes, and 248 MiB of
  double-quoted scalars of `\P` escapes, whose text is 1.5 times their
  input. The same tree with a file of 496,000 schema errors in place of
  one 4 MB file: 529 MB. Errors cost no memory beyond the first 2,000 of a
  stage, as stage 3 gives each error to the report at once (a `oneOf`
  branch holds at most 1,000 and a count). Real trees are a few MB.
  `scada-conf` runs validation of an untrusted tree under a memory limit of
  its own sized from this figure (#102).

## Interfaces

- `include/scada/conf/diagnostic.h`: `Diagnostic`, `Diagnostics`, `Report`.
- `include/scada/conf/tree.h`: `classify_path`, `scan_tree`, `Tree`.
- `include/scada/conf/node.h`: `Document`, `DocumentBuilder`, `Entry`,
  `SourcePosition`, the tree of parsed files.
- `include/scada/conf/yaml.h`: `parse_yaml`, the subset parser.
- `include/scada/conf/scalar.h`: the text forms of integers, decimals,
  booleans, durations, sizes and percentages (contract 1.2, 2.2).
- `include/scada/conf/names.h`: the name grammars of section 2.1 and the
  address and time forms the schemas check.
- `include/scada/conf/expression.h`: the lexer and parser of the expression
  grammar (contract 5.6), giving a tree or one located error.
- `include/scada/conf/schema.h`: `SchemaSet`, the compiled schema files and
  the validator of stage 3.
- `include/scada/conf/expand.h`: `expand_tree` and `ExpandedTree`, stage 4.
- `include/scada/conf/references.h`: `check_references`, the part of stage 5
  that needs no point tags.
- `include/scada/conf/types.h`: `ScalarType` and `ScalarTypes`, the types
  stage 3 gives the scalars of a Document.
- `include/scada/conf/read.h`: typed reads of a scalar node (`to_int64`,
  `to_uint64`, `to_decimal`, `to_bool`, `to_duration_ns`, `to_raw_pattern`,
  `is_integer_form`), each a `Result`, for later stages and the drivers.

## The schema files

`schemas/*.schema.json` are JSON Schema 2020-12, one per file type, from the
tables of the contract, which win where the two disagree (D-69). They are
read with the YAML subset parser and built into the library at configure
time (`CMakeLists.txt`). The keywords the validator reads, each with its
code, and the project keywords `x-scada-*` are listed in contract Annex C;
to another JSON Schema validator the `x-scada-*` keywords are annotations,
so it checks structure only. A schema file this validator cannot compile is
a defect of the software, found by `BuiltinSchemas.AllCompile`.
- `include/scada/conf/validate.h`: `validate_tree`, the entry point that runs
  the stages.
- Namespace `scada::conf`; CMake target `scada::conf`.

```cpp
#include "scada/conf/validate.h"

const scada::Result<scada::conf::Report> report =
    scada::conf::validate_tree("/var/lib/scada/conf/staging");
if (report.ok()) {
    for (const scada::conf::Diagnostic& d : report.value().errors) {
        std::cout << scada::conf::format(d) << '\n';
    }
}
```

`validate_tree` fails only when the tree cannot be read at all; a wrong tree
gives a report with errors.

## How to test it

```
ctest --preset dev -R 'scada-conf|_fuzz-corpus'
```

- `tests/`: unit tests. `tree_scan_test.cpp` builds trees in a temporary
  directory, for what git cannot hold (links, FIFOs, sockets, large files,
  20,000 entries).
- `tests/corpus/`: the conformance corpus of contract section 14, run by
  `corpus_test.cpp`; its README gives the format.
- `tests/yaml_test.cpp`: every construct of the subset, accepted and refused,
  with its code, line and column, and each parsing limit.
  `tests/yaml_limits_test.cpp` holds the default limits at N and N + 1 (a
  file of 8 MiB, a million nodes, 65,536 entries), an executable of its
  own with its own time limit (`scada-conf-yaml-limits-test`, #164); the
  helpers both use are in `tests/yaml_cases.h`.
- `fuzz/`: fuzz targets with their seed corpora (`ctest --preset fuzz` in the
  `fuzz` build): `path_class_fuzz` (stage 1 paths), `yaml_fuzz` (the
  parser; its seeds hold the contract's examples and every refused
  construct), `schema_fuzz` (stage 3 over any file that parses) and
  `expression_fuzz` (the expression grammar against a second writing of
  it with its own lexer: both must accept the same texts, refuse with the
  same code and column, and build the same tree; its seeds are the cases
  of conformance test 11 and the limits),
  `scalar_fuzz` (the scalar forms against a second writing of their
  grammars) and `tree_fuzz` (`validate_tree` over small trees seeded with
  the corpus).
  `yaml_fuzz` checks that a parse gives a document or exactly one error
  inside the input, that no limit is exceeded, that every node lies inside
  the input, and that a document written back in the subset parses to the
  same tree.
- `tests/schema_test.cpp`: each keyword of the validator with its code, and
  the built-in schemas against the contract's examples.
  `tests/schema_limits_test.cpp` checks that typed keys cost linearithmic
  time, over files of 4,096 and 65,536 keys, in an executable of its own
  (`scada-conf-schema-limits-test`, #164); `tests/schema_cases.h` holds
  the schema set both build.
- `tests/expand_test.cpp`: each step of the merge order and each CF-4xx
  code, with the instance and the template line of its errors.
- `tests/limits_test.cpp`, `tests/tree_limits_test.cpp` and
  `tests/expression_limits_test.cpp`: the point limit of an instance
  (CF-414), the limits of the expanded tree (nodes, tags, the work budget
  and its weighting of expressions) at their edges, and the parse of each
  expression once for all its evaluations. Each makes millions of nodes or
  units of work, so each is an executable of its own
  (`scada-conf-limits-test`, `scada-conf-checked-values-limits-test` for
  the bound of the checked values of D-115, `scada-conf-tree-limits-test`,
  `scada-conf-expression-limits-test`) with its own time limit; the cases
  they share are in `tests/expand_cases.h`.
- `tests/config_eval_test.cpp`: the values of configuration expressions and
  the failures that are CF-410.
- `tests/references_test.cpp`: each check of stage 5 on a whole tree.
- `tests/tag_names_test.cpp`: the tags of the expanded tree (point, bit and
  generated tags and their types, CF-301, CF-302, CF-403, CF-1001), the
  references to them outside expressions (CF-310, CF-311, CF-312, CF-604),
  and one cause at many instances given once. `tests/tag_names_prefix_test.cpp`:
  the tags under a prefix, in the order of names, for the patterns of
  run-time expressions.
- `tests/runtime_expressions_test.cpp`: the run-time expressions against
  the tags (D-117): types, cycles of virtual tags and interlocks, task
  windows, BOOL results, grouping, and what is kept.
  `tests/runtime_limits_test.cpp` (`scada-conf-runtime-limits-test`, an
  executable of its own) bounds the work of patterns over tens of
  thousands of tags: each pattern a vertex walked once, resolved once for
  all its uses.
- `tests/expression_types_test.cpp`: each row of the operator and function
  tables of contract 5.6 for configuration expressions.
