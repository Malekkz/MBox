# libscada-conf

Validation of the configuration tree, as the configuration schemas contract
defines it (`docs/contracts/config-schemas.md`, issue #100, P2-03). It is
built in slices; this README lists what each part does now.

## What it does

- Collects diagnostics (errors `CF-<n>`, warnings `CW-<n>`) with their file,
  line and column, in the order of contract section 1.4, keeping the first
  1,000 of each and a count (section 1.3).
- Stage 1 of validation (section 1.4): reads a tree without following links
  and without opening anything outside its root, and refuses paths the
  contract does not list (CF-101), a missing `system.yaml` (CF-103) and files
  or trees over their size limits (CF-113).
- Stage 2: parses every YAML file of the tree with the project's own parser
  of the YAML subset of section 1.1 (below) into a `Document` of `Node`s that
  keep each scalar's text, its quoting style, and its file, line and column.
  A `.st` file is not parsed in phase 2: it is held to the limits of stage 1
  only (D-106).

Not yet: schemas (stage 3) and the cross-file rules (stage 5) follow in later
slices of #100.

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
| The reserved indicators `@` and `` ` ``, an unknown escape, an escape giving U+0000 or a surrogate | CF-110 |
| Any other text outside the grammar: bad indentation, an unterminated quote, an unclosed bracket, text after a value | CF-110 |
| Two keys of one map with the same text (`a` and `'a'`) | CF-111 |
| `null`, `Null`, `NULL`, `~` as plain values, an empty value, an empty sequence item | CF-112 |
| A file over 8 MiB, more than 1,000,000 nodes in a file or 4,000,000 in the tree, more than 32 nested collections, more than 65,536 entries in a collection, a scalar over 64 KiB, a key over 64 bytes | CF-113 |
| Invalid UTF-8 (overlong forms, surrogates, above U+10FFFF, cut short), NUL, a carriage return without a line feed, other C0 control characters, DEL, C1 control characters | CF-114 |
| A tab anywhere but in a comment, a quoted scalar or a block scalar's content | CF-114 |

Scalars are not typed here: `yes`, `010` and `0x10` are text, and their type
is decided by the schema of their key (contract 1.2, stage 3). Keys that are
equal only once typed (`16` and `0x10`) are found by stage 3 (CF-111). Template expansion (stage 4) is
#103, the tag set (stage 6) #101, the protocols' check mode (stage 7) #102.

## Interfaces

- `include/scada/conf/diagnostic.h`: `Diagnostic`, `Diagnostics`, `Report`.
- `include/scada/conf/tree.h`: `classify_path`, `scan_tree`, `Tree`.
- `include/scada/conf/node.h`: `Document`, `Node`, the tree of one parsed
  file.
- `include/scada/conf/yaml.h`: `parse_yaml`, the subset parser.
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
ctest --preset dev -R 'scada-conf|path_class_fuzz|yaml_fuzz'
```

- `tests/`: unit tests. `tree_scan_test.cpp` builds trees in a temporary
  directory, for what git cannot hold (links, FIFOs, sockets, large files,
  20,000 entries).
- `tests/corpus/`: the conformance corpus of contract section 14, run by
  `corpus_test.cpp`; its README gives the format.
- `tests/yaml_test.cpp`: every construct of the subset, accepted and refused,
  with its code, line and column, and each parsing limit.
- `fuzz/`: fuzz targets with their seed corpora (`ctest --preset fuzz` in the
  `fuzz` build): `path_class_fuzz` (stage 1 paths) and `yaml_fuzz` (the
  parser; its seeds hold the contract's examples and every refused
  construct).
