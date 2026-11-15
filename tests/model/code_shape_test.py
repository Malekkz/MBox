#!/usr/bin/env python3
"""Tests of code_shape.py on a disassembly of the tag library.

The fixture is the output of `llvm-objdump -d -r -C --no-show-raw-insn` for
tag_record.cpp and change_ring.cpp compiled by Clang 18 for ARM64 with -O2
and -mbranch-protection=standard, cut to the six functions the script
checks. Each test below edits one function the way a change of the C++
would (a fence dropped or moved, an acquire or release made relaxed) and
expects the script to report it. Runs on any host.
"""

import pathlib
import re
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import code_shape  # noqa: E402  (after the path is set)

FIXTURE = HERE / "fixtures" / "clang18-aarch64-O2.txt"

WRITERS = ("write_record", "write_string_record", "append_change")
READERS = ("read_record", "read_string_record", "read_change")


def blocks(text):
    """Splits the disassembly into (function name, lines) blocks."""
    result = []
    for line in text.splitlines():
        header = code_shape.FUNCTION.match(line)
        if header:
            result.append([header.group(2), []])
        if result:
            result[-1][1].append(line)
    return result


def edit(text, function, change):
    """Applies `change` to the lines of the one function named `function`."""
    out = []
    matched = 0
    for name, lines in blocks(text):
        if name.startswith(f"scada::tag::{function}("):
            matched += 1
            lines = change(list(lines))
        out.extend(lines)
    assert matched == 1, function
    return "\n".join(out) + "\n"


def is_instruction(line, mnemonic, operand=None):
    match = code_shape.INSTRUCTION.match(line)
    if not match or match.group(2) != mnemonic:
        return False
    return operand is None or match.group(3).split()[0] == operand


def drop(mnemonic, operand=None):
    def change(lines):
        kept = [l for l in lines if not is_instruction(l, mnemonic, operand)]
        assert len(kept) < len(lines)
        return kept
    return change


def rename(old, new):
    def change(lines):
        result = [re.sub(rf"\t{old}\t", f"\t{new}\t", l) for l in lines]
        assert result != lines
        return result
    return change


def move(mnemonic, operand, next_to, after):
    """Moves the first `mnemonic operand` next to the first `next_to`."""
    def change(lines):
        index = next(i for i, l in enumerate(lines)
                     if is_instruction(l, mnemonic, operand))
        moved = lines.pop(index)
        target = next(i for i, l in enumerate(lines)
                      if is_instruction(l, next_to))
        lines.insert(target + 1 if after else target, moved)
        return lines
    return change


# A writer in the shape GCC 13 gives at -O2 with -D_GLIBCXX_ASSERTIONS: the
# field stores in a helper it did not inline, and a cold path that repeats
# the start of the write and ends in a failed library assertion.
GCC_STYLE_WRITER = """\
tag_record.cpp.o:     file format elf64-littleaarch64

0000000000000000 <scada::tag::(anonymous namespace)::store_fields(int)>:
       0:\tstrb\tw1, [x0, #5]
       4:\tstrh\tw2, [x0, #6]
       8:\tstr\tx3, [x0, #8]
       c:\tstr\tx4, [x0, #16]
      10:\tstr\tx5, [x0, #24]
      14:\tstr\tx6, [x0, #32]
      18:\tret
000000000000001c <scada::tag::write_record(scada::tag::TagRecord&, int)>:
      1c:\tcbz\tx1, 38 <scada::tag::write_record(scada::tag::TagRecord&, int)+0x1c>
      20:\tldr\tw9, [x0]
      24:\tstr\tw10, [x0]
      28:\tdmb\tish
      2c:\tbl\t0 <scada::tag::(anonymous namespace)::store_fields(int)>
      30:\tstlr\tw9, [x0]
      34:\tret
      38:\tstr\tw10, [x0]
      3c:\tdmb\tish
      40:\tbl\t0 <scada::tag::write_record(scada::tag::TagRecord&, int)>
\t\t\t40: R_AARCH64_CALL26\tstd::__glibcxx_assert_fail(char const*, int, char const*, char const*)
"""


def check_gcc_style_writer(text):
    program = code_shape.Program(text)
    (function,) = program.find("scada::tag::write_record(")
    return code_shape.check_function(program, function, "writer", (True, 6))


def insert_after(address, line):
    """Inserts `line` after the instruction at `address` (as printed)."""
    def change(lines):
        index = next(i for i, l in enumerate(lines)
                     if l.strip().startswith(address))
        lines.insert(index + 1, line)
        return lines
    return change


class CodeShapeTest(unittest.TestCase):

    def setUp(self):
        self.text = FIXTURE.read_text(encoding="utf-8")

    def assert_caught(self, function, change):
        failures = code_shape.check(edit(self.text, function, change))
        self.assertTrue(
            any(f.startswith(f"scada::tag::{function}(") for f in failures),
            failures)

    def test_the_library_as_compiled_has_the_shape(self):
        self.assertEqual(code_shape.check(self.text), [])

    def test_a_missing_function_is_reported(self):
        text = edit(self.text, "read_change", lambda lines: [])
        self.assertEqual(len(code_shape.check(text)), 1)

    def test_a_writer_without_its_fence_fails(self):
        for function in WRITERS:
            with self.subTest(function):
                self.assert_caught(function, drop("dmb", "ish"))

    def test_a_writer_with_its_fence_moved_to_the_end_fails(self):
        for function in WRITERS:
            with self.subTest(function):
                self.assert_caught(
                    function, move("dmb", "ish", "stlr", after=False))

    def test_a_writer_with_a_relaxed_end_store_fails(self):
        for function in WRITERS:
            with self.subTest(function):
                self.assert_caught(function, rename("stlr", "str"))

    def test_a_reader_without_its_fence_fails(self):
        for function in READERS:
            with self.subTest(function):
                self.assert_caught(function, drop("dmb", "ishld"))

    def test_a_reader_with_its_fence_moved_before_the_copy_fails(self):
        for function in READERS:
            with self.subTest(function):
                self.assert_caught(
                    function, move("dmb", "ishld", "ldar", after=True))

    def test_a_reader_with_a_relaxed_first_load_fails(self):
        for function in READERS:
            with self.subTest(function):
                self.assert_caught(function, rename("ldar", "ldr"))

    def test_a_reader_with_a_path_around_its_fence_fails(self):
        # A branch from the last copy load to the second load of seq.
        self.assert_caught("read_record", insert_after(
            "4c:", "      4e:      \tcbz\tw18, 0x54 <read_record+0x54>"))

    def test_a_reader_that_accepts_without_the_check_fails(self):
        # A branch from the last copy load straight to the stores of the
        # result: no fence and no second load.
        self.assert_caught("read_record", insert_after(
            "4c:", "      4e:      \tcbz\tw18, 0x60 <read_record+0x60>"))

    def test_a_reader_with_a_load_after_the_check_fails(self):
        # A field loaded after the second load of seq, outside the window.
        self.assert_caught("read_record", insert_after(
            "54:", "      56:      \tldr\tx3, [x0, #0x8]"))

    def test_a_writer_with_a_store_before_the_odd_sequence_fails(self):
        # A field stored before `seq` is odd, outside the window.
        self.assert_caught("write_record", insert_after(
            "2b0:", "     2b2:      \tstr\tx9, [x8, #0x8]"))

    def test_a_call_that_is_not_panic_fails(self):
        def add_call(lines):
            index = next(i for i, l in enumerate(lines)
                         if is_instruction(l, "dmb", "ish"))
            lines.insert(index + 1, "     100:      \tbl\t0x0 <x>")
            lines.insert(index + 2,
                         "\t\t0000000000000100:  R_AARCH64_CALL26\tother()")
            return lines
        self.assert_caught("write_record", add_call)


class GccStyleTest(unittest.TestCase):

    def test_helpers_are_followed_and_paths_that_do_not_return_left_out(self):
        self.assertIsNone(check_gcc_style_writer(GCC_STYLE_WRITER))

    def test_a_helper_without_its_stores_fails(self):
        text = GCC_STYLE_WRITER.replace("str\tx6, [x0, #32]", "nop")
        self.assertIn("fewer than 6 stores", check_gcc_style_writer(text))

    def test_a_call_that_is_not_found_fails(self):
        text = GCC_STYLE_WRITER.replace(
            "bl\t0 <scada::tag::(anonymous namespace)::store_fields(int)>",
            "bl\t0 <elsewhere()>")
        self.assertIn("unexpected instruction (C)",
                      check_gcc_style_writer(text))

    def test_a_cold_path_that_returns_is_checked(self):
        text = GCC_STYLE_WRITER.replace(
            "\tbl\t0 <scada::tag::write_record(scada::tag::TagRecord&, int)>\n"
            "\t\t\t40: R_AARCH64_CALL26\tstd::__glibcxx_assert_fail("
            "char const*, int, char const*, char const*)\n",
            "\tret\n")
        self.assertIn("release store", check_gcc_style_writer(text))


if __name__ == "__main__":
    unittest.main()
