# check

The `check` build target (coding standard, section 13): it runs the formatter,
the linter, the banned-function check and the `SCADA_RT` check over all C++
sources and fails on any finding. Given a base revision, clang-tidy lints
only the translation units a change could have affected; see "Linting only
what changed".

## What it does

| Check | Tool | Rules | Reported as |
| --- | --- | --- | --- |
| `format` | clang-format | `.clang-format` | `-Wclang-format-violations` |
| `tidy` | clang-tidy | `.clang-tidy`; every finding is an error | the clang-tidy check name |
| `banned` | clang-query | Coding standard, section 12 | `scada-banned` |
| `rt` | clang-query | Deny-list for `SCADA_RT` functions | `scada-rt` |

- It checks every `.h`, `.hpp`, `.cpp`, `.cc` and `.cxx` file under `libs/`,
  `services/`, `tools/` and `tests/`, except the fixtures of its own tests
  and the deliberate bugs in `tests/sanitizer-fixtures/`.
- `tidy`, `banned` and `rt` use the compile commands of the build directory.
  A source file that is not part of the build, or does not compile with
  Clang, fails the check. Headers are checked through the files that include
  them.
- `rt` reports, inside a function marked `SCADA_RT` or a lambda in it:
  `new`, `delete`, the C allocation functions, `std::make_unique`,
  `std::make_shared`, `std::allocate_shared`, C stream output such as
  `printf`, locking a standard mutex, constructing a standard lock object, and
  `pthread_mutex_lock`.
- `rt` finds direct calls only. It does not follow calls into other
  functions, and it does not yet know blocking calls, system calls or C++
  stream output (`docs/decisions.md`, D-9).
- It does not change any file. To apply the layout, run
  `clang-format -i <file>`.

## Linting only what changed

`format`, `banned` and `rt` always check every file. `tidy` lints every
translation unit unless a base revision is given, with the CMake option
`SCADA_CHECK_TIDY_BASE` or the script's `--tidy-base`. Then it lints
(`lint_selection.py`, decision D-67):

- every changed translation unit, and every unit whose dependencies contain
  a changed file. The dependencies are the compiler's own output: the
  unit's compile command from `compile_commands.json` with `-M` instead of
  its output options. A header included through other headers counts.
- everything, when one of the files of `FULL_LINT_PATTERNS` changed
  (`.clang-tidy`, `CMakeLists.txt`, `*.cmake`, the presets, `cmake/`,
  `third-party/`, `.github/workflows/`, `.github/actions/`, `tools/check/`,
  `*.in`), when a header was deleted or renamed (its former dependents are
  not in the dependencies after the change), and when the changes or the
  dependencies cannot be worked out: no merge base, a shallow clone, a unit
  that does not preprocess, any error of the selection.

Changed means: between the merge base of the revision and `HEAD`, and the
work tree, untracked files included. The first line of the `tidy` output
names the mode, the reason and the number of units, then the units linted:

```
check: tidy: changed files (3 files changed since the merge base 4c22719a1b2c of origin/main and HEAD): 2 of 165 translation units
check: tidy: everything (tools/check/scada_check.py changed, which sets the rules): 165 of 165 translation units
```

The pipeline sets the option on a pull request only. A push to `main` and a
manual run lint everything.

## Suppressing a finding

Only on the line of the finding, naming the check and giving a reason (coding
standard, section 14):

```cpp
std::memcpy(target.data(), source.data(), size);  // NOLINT(scada-banned): reviewed helper, sizes checked above
```

`// NOLINTNEXTLINE(scada-banned): reason` on the line before also works.
`banned` and `rt` ignore a `NOLINT` that does not name them.

One expression is exempt without a suppression: the `new` inside
GoogleTest's macro `GTEST_TEST_`, which `TEST` and `TEST_F` expand to in
every test file. A `new` in a test body is still reported.

## How to run it

Needs Python 3.8 or newer and clang-format, clang-tidy and clang-query 18 or
newer. CMake looks for them on `PATH`; set `SCADA_CLANG_FORMAT`,
`SCADA_CLANG_TIDY` or `SCADA_CLANG_QUERY` to use another path.

```
cmake --preset dev
cmake --build --preset dev --target check
```

The script can also be run on single files or with single checks:

```
python3 tools/check/scada_check.py --help
python3 tools/check/scada_check.py --source-dir . --build-dir build/dev \
    --clang-format clang-format --only format libs/scada-hello/src/hello.cpp
```

Exit status: 0 no findings, 1 findings, 2 the checks could not run.

## Changing the rules

- Layout: `.clang-format`. Lint checks and naming: `.clang-tidy`. A disabled
  clang-tidy check needs a one-line reason there and an entry in
  `docs/decisions.md`.
- Banned list and `SCADA_RT` deny-list: `BANNED_RULES` and `RT_RULES` in
  `scada_check.py`. Add a line to a fixture in `tests/fixtures/` for every new
  rule.

## How to test it

```
ctest --preset dev -R scada-check-test
```

The test runs the script on the files in `tests/fixtures/`, which break the
rules on purpose. It takes about 15 s, most of it in clang-tidy.

```
ctest --preset dev -R scada-check-selection-test
```

The self-test of the selection: a small project in a temporary directory,
its dependencies from the build's C++ compiler, and a git repository. It
covers a changed source file, a header with two dependents, a header
included through another header, a changed `.clang-tidy`, a deleted header,
documents only, the errors that lint everything, and a fault planted in a
header that fails an unchanged unit including it. About 5 s.
