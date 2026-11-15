# tests/model

Conformance test 4 of the tag database contract: the sequence lock
(sections 6.1, 6.2 and 7) and the change ring (sections 8.1 and 8.2) checked
against the Armv8 memory model. Decision: `docs/decisions.md`, D-37.

x86-64 hides ordering mistakes, the ThreadSanitizer build leaves the fences
out, and the tests with derived values only sample. This directory checks
the order of the accesses in two parts:

1. **The model.** `seqlock.litmus` and `change-ring.litmus` are litmus tests
   for herd7 with the Armv8 model `aarch64.cat`. Each writes the instructions
   `libs/scada-tag/src/tag_record.cpp` and `change_ring.cpp` compile to, and
   its condition is a read the library would accept with a copy that mixes
   writes. `model_check.py` expects herd7 to find that condition
   unreachable, and reachable in each of four copies of the test that lack
   one ordering: `fence_release` (`DMB ISH`), `fence_acquire`
   (`DMB ISHLD`), the release store that ends a write (`STLR`) and the
   acquire load that starts a read (`LDAR`). So each of the four is needed,
   and with all four no execution of the test gives a mixed copy. A test
   covers two writes against one read attempt (seqlock) and three appends
   against one read with an overwrite (ring); herd7 explores every
   execution of those under the model, not longer runs.
2. **The code.** `code_shape.py` disassembles the six protocol functions
   (`write_record`, `write_string_record`, `read_record`,
   `read_string_record`, `append_change`, `read_change`) from the ARM64 tag
   library and checks, along every path from the entry to a return, that
   their barriers and acquire and release accesses come in the order the
   litmus tests model: one `DMB ISH` after the odd sequence number and
   before the field stores, one release store after them and no store after
   it; each read attempt an acquire load, the copy, `DMB ISHLD` and the
   second load. A minimum count of loads or stores between them stands for
   the copy, since compilers do not merge atomic accesses. Calls of helpers
   the compiler did not inline (GCC 13 at -O2 inlines few of them) are
   followed; a path that ends in `panic`, a failed library assertion or the
   stack protector is left out, since it publishes nothing. Each branch is
   taken at most twice, so a retry loop runs once or twice. Without this
   part an edit that drops a fence would leave the model passing. When it
   fails it prints the disassembly of the functions concerned.

What part 2 does not see: which register holds which field. A copy load
moved after `DMB ISHLD`, while the fence stays after enough other loads,
passes it. Reviewers check the orders in `tag_record.cpp` and
`change_ring.cpp` against the contract, as the comments there ask.

## Tests

| Test | Label | Registered when |
| --- | --- | --- |
| `scada-model-check` | `model` | herd7 is found (`SCADA_HERD7`) |
| `scada-model-code-shape-test` | `unit` | ARM64 build with optimisation and without a sanitizer (`release`, `perf`, `release-arm64`) |
| `scada-model-code-shape-self-test` | `unit` | always: `code_shape.py` on a disassembly of Clang 18 output, as compiled and with each ordering edited out |

The pipeline job `model check` configures `dev` with
`-DSCADA_REQUIRE_HERD7=ON`, which stops if herd7 is missing, and runs
`ctest --preset model`. `build and test (ARM64)` runs the code check as
part of `ctest --preset release`.

## Running it

```
cmake --preset dev -DSCADA_HERD7=<path of herd7>
ctest --preset model --verbose
```

`--verbose` prints one line for each run of herd7, for example
`ok   seqlock, fence_release (DMB ISH) left out: Sometimes (expected Sometimes)`.
`python3 tests/model/model_check.py --herd7 <path> --verbose` prints the
whole output of herd7, with the states it found.

## Getting herd7

herd7 is part of herdtools7 (CeCILL-B; `aarch64.cat` is BSD-3-Clause, Arm
Ltd), version 7.58 as listed in `third-party/dependencies.json`. It needs
OCaml 4.08 or newer, dune, menhir and zarith. `opam install herdtools7`
installs it; `.github/actions/setup-build-machine/action.yml` (input
`herd7`) builds it from the tag instead, with the OCaml packages of the
runner. herd7 only runs on the build machine; nothing of it is linked or
shipped.

## Changing the protocol

A change of the order of accesses in `tag_record.cpp` or `change_ring.cpp`
changes the litmus test in the same pull request, and the fixture
`fixtures/clang18-aarch64-O2.txt` if the self-test no longer matches. The
fixture is the output of
`llvm-objdump -d -r -C --no-show-raw-insn` for the two files compiled by
Clang 18 for `aarch64-linux-gnu` with `-std=c++20 -O2 -DNDEBUG
-mbranch-protection=standard`, cut to the six functions.
