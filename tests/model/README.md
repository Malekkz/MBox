# tests/model

Conformance test 4 of the tag database contract: the sequence lock
(sections 6.1, 6.2 and 7), the change ring (sections 8.1 and 8.2) and the
repair after an owner restart (section 6.3) checked against the Armv8
memory model. Decisions: `docs/decisions.md`, D-37 and D-39.

x86-64 hides ordering mistakes, the ThreadSanitizer build leaves the fences
out, and the tests with derived values only sample. This directory checks
the order of the accesses in two parts:

1. **The model.** `seqlock.litmus`, `change-ring.litmus` and
   `repair.litmus` are litmus tests for herd7 with the Armv8 model
   `aarch64.cat`. Each writes the instructions
   `libs/scada-tag/src/tag_record.cpp`, `change_ring.cpp` and
   `attachment.cpp` compile to, and its condition is a read the library
   would accept with a copy that mixes writes. `model_check.py` expects
   herd7 to find that condition unreachable, and reachable in each of four
   copies of the test that lack one ordering: `fence_release`
   (`DMB ISH`), `fence_acquire` (`DMB ISHLD`), the release store that ends
   a write (`STLR`) and the acquire load that starts a read (`LDAR`). So
   each of the four is needed, and with all four no execution of the test
   gives a mixed copy. A test covers two writes against one read attempt
   (seqlock), three appends against one read with an overwrite (ring), and
   a write that stops after its fence, then the repair, against one read
   (repair); herd7 explores every execution of those under the model, not
   longer runs.
2. **The code.** `code_shape.py` disassembles the seven protocol functions
   (`write_record`, `write_string_record`, `read_record`,
   `read_string_record`, `append_change`, `read_change`,
   `repair_tag_records`) from the ARM64 tag library and checks, along
   every path from the entry to a return, that their barriers and acquire
   and release accesses come in the order the litmus tests model:
   - a sequence-lock write: one store before `DMB ISH` (the odd sequence
     number), the field stores, one release store to the same address,
     no store after it; an append the same without the first store;
   - a read attempt: the acquire load, the copy, `DMB ISHLD`, then a first
     load of the address of the acquire load (the second load), and no
     load of shared memory after that; an attempt that loads any shared
     memory without reaching the fence fails, and loads of the caller's
     own memory (the cursor) may come anywhere;
   - the repair: at least two stores (quality and flags) before each
     release store, no store after the last, no fence.

   Along each path the registers and the 8-byte stack slots are followed
   from the entry, so each access is known as an offset from an argument
   or from the stack pointer. The arguments that point at shared memory
   are listed for each function (the record, the string slot, the ring);
   the caller's own memory (the cursor of a ring read) may be loaded after
   the second load. Accesses of the function's own frame, through `sp`,
   `x29` or a copy of either, are left out. A minimum count of loads or
   stores of shared memory stands for the copy, since compilers do not
   merge atomic accesses. Calls of helpers the compiler did not inline
   (GCC 13 at -O2 inlines few of them) and jumps into a part GCC split off
   (`[clone .cold]`) are followed; a path that ends in `panic`, a failed
   library assertion or the stack protector is left out, since it
   publishes nothing. Each branch is taken at most twice (once in the
   repair, a loop over all records), so a retry loop runs once or twice.
   Without this part an edit that drops a fence would leave the model
   passing. When it fails it prints the disassembly of the functions
   concerned.

   `code_shape_mutants.py` compiles 22 edited copies of the three sources
   with the commands of the build (`compile_commands.json`), so with its
   compiler and flags, and expects the check to reject each: each fence
   dropped or moved, each acquire or release access made relaxed, a second
   load of another field, a load after the second load, a store after the
   end of a write or of the repair, a fence in the repair. Only a finding
   about the order counts, not a function the check cannot find or walk.
   It also compiles each source unchanged and expects it to pass.

Known limits (D-37, D-39):

- Part 2 checks the addresses of the sequence number and of `write_seq`,
  and that every other load of a read attempt comes between the acquire
  load and the fence; it does not know which field each copy load or field
  store is. A copy that loads one field twice and another not at all
  passes. Reviewers check the orders in `tag_record.cpp`,
  `change_ring.cpp` and `attachment.cpp` against the contract, as the
  comments there ask.
- A stronger ordering fails part 2 too (an acquire second load, `DMB ISH`
  in a reader): change the litmus tests and the check with it.
- The model is bounded: two writes against one read attempt, three appends
  against one read, one repair against one read.
- The store of the ring's `epoch` at reattach (`src/attachment.cpp`) is in
  neither part: it is one counter that publishes nothing else (D-39).

## Tests

| Test | Label | Registered when |
| --- | --- | --- |
| `scada-model-check` | `model` | herd7 is found (`SCADA_HERD7`); only `ctest --preset model` runs it |
| `scada-model-code-shape-test` | `unit` | ARM64 build of type `Release`, `RelWithDebInfo` or `MinSizeRel` without a sanitizer (`release`, `perf`, `release-arm64`) |
| `scada-model-code-shape-mutants-test` | `unit` | as `scada-model-code-shape-test` |
| `scada-model-code-shape-self-test` | `unit` | always: `code_shape.py` on a disassembly of Clang 18 output, as compiled and with each ordering edited out, and on short pieces in the shapes GCC gives |

The pipeline job `model check` configures `dev` with
`-DSCADA_REQUIRE_HERD7=ON`, which stops if herd7 is missing, and runs
`ctest --preset model`. `build and test (ARM64)` runs the code check and
the edited sources as part of `ctest --preset release`, with GCC.

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

herd7 is part of herdtools7 (CeCILL-B, allowed for scope `build` and
`test` only; `aarch64.cat` is BSD-3-Clause, Arm Ltd), at the version and
commit listed in `third-party/dependencies.json`. It needs OCaml 4.08 or
newer, dune, menhir and zarith. `opam install herdtools7` installs it;
`.github/actions/setup-build-machine/action.yml` (input `herd7`) builds it
from the tag instead, with the OCaml packages of the runner, checks the
commit, and keeps the build in the Actions cache under the commit and the
runner image. herd7 only runs on the build machine; nothing of it is
linked or shipped.

## Changing the protocol

A change of the order of accesses in `tag_record.cpp`, `change_ring.cpp`
or the repair in `attachment.cpp` changes the litmus test in the same pull
request, the fixture `fixtures/clang18-aarch64-O2.txt` if the self-test no
longer matches, and the edits of `code_shape_mutants.py` if their text is
no longer in the source (the test says so). The fixture is the output of
`llvm-objdump -d -r -C --no-show-raw-insn` for the three files compiled by
Clang 18 for `aarch64-linux-gnu` with `-std=c++20 -O2 -DNDEBUG
-mbranch-protection=standard` and the include directories of the tag
library, cut to the seven functions and the helpers they call.
