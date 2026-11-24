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

Stage 5 runs when stages 1 to 3 found no error. Stage 4, the expansion of
templates and instances, comes with #103, so stage 5 checks for now what
needs no expansion (`check_references`, `include/scada/conf/references.h`,
D-113):
- names that must exist (CF-310): templates and `extends`, a template's
  protocol, areas, logging policies and aggregate tiers, enumerations (of
  the template's chain or `enums.yaml`), tasks, programs, interlocks,
  outputs, alarm priorities and classes;
- names given twice: channels, devices and interlocks (CF-303), and the tags
  of tag files and interlocks (CF-301, CF-302);
- the Modbus channel rules of Annex A.1 (CF-622);
- the types of a template's configuration expressions (CF-523), and a name
  in them that no parameter or variable of the chain has (CF-310).

A reference to a tag gives no error yet: point tags exist only once
instances are expanded, so those references, and the names and types of
run-time expressions, are checked in slice 7 of #100, after #103.

Not in phase 2: structured text is not parsed, and the block library
declares no block type until phase 6, so a tree with control programs does
not validate before phase 6.

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
- `include/scada/conf/references.h`: `check_references`, the part of stage 5
  that needs no expansion.
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
- `tests/references_test.cpp`: each check of stage 5 on a whole tree, and
  that a reference to a tag gives no error before the expander exists.
- `tests/expression_types_test.cpp`: each row of the operator and function
  tables of contract 5.6 for configuration expressions.
