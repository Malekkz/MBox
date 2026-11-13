# check

The `check` build target (coding standard, section 13): it runs the formatter,
the linter, the banned-function check and the `SCADA_RT` check over all C++
sources and fails on any finding.

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
