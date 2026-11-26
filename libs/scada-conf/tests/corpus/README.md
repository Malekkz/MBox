# Conformance corpus of the configuration schemas contract

The corpus of contract section 14 (`docs/contracts/config-schemas.md`). Each
directory is one case:

| Path | Content |
| --- | --- |
| `<case>/tree/` | The configuration tree, as it would be committed. |
| `<case>/expected` | The diagnostics the tree must give, one per line, in the order of contract section 1.4: `<file>:<line>:<column>: <code>`. Lines that start with `#` are comments. A case whose list is empty is a valid tree with no warning. |
| `<case>/expanded` | Optional, for a valid tree: its expanded devices and points (contract 14, test 6), one per line in the order of expansion: `<device>: {<keys>}`, then `<device>.<point>: {<keys>}`, values in flow style as their text. |

Errors are listed when the tree has any; otherwise its warnings. Messages are
not pinned here; the unit tests check them.

Case names start with the stage that gives their codes (`s1-` for stage 1),
so that a case stays valid when later stages are added: a tree whose errors
are in stage 1 never reaches stage 2.

Cases that git cannot hold (a symbolic link, a FIFO, a socket, a file of
8 MiB, a tree of 20,000 files) are unit tests in `../tree_scan_test.cpp`,
which build the tree in a temporary directory.

`scada-conf-unit-test` runs every case: `ctest --preset dev -R scada-conf`.
