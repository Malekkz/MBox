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

Not yet: parsing (stage 2), schemas (stage 3) and the cross-file rules
(stage 5) follow in later slices of #100. Template expansion (stage 4) is
#103, the tag set (stage 6) #101, the protocols' check mode (stage 7) #102.

## Interfaces

- `include/scada/conf/diagnostic.h`: `Diagnostic`, `Diagnostics`, `Report`.
- `include/scada/conf/tree.h`: `classify_path`, `scan_tree`, `Tree`.
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
ctest --preset dev -R 'scada-conf|path_class_fuzz'
```

- `tests/`: unit tests. `tree_scan_test.cpp` builds trees in a temporary
  directory, for what git cannot hold (links, FIFOs, sockets, large files,
  20,000 entries).
- `tests/corpus/`: the conformance corpus of contract section 14, run by
  `corpus_test.cpp`; its README gives the format.
- `fuzz/`: fuzz targets with their seed corpora (`ctest --preset fuzz` in the
  `fuzz` build).
