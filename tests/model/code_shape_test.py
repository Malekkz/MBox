#!/usr/bin/env python3
"""Tests of code_shape.py on a disassembly of the tag library.

The fixture is the output of `llvm-objdump -d -r -C --no-show-raw-insn` for
tag_record.cpp, change_ring.cpp and attachment.cpp compiled by Clang 18 for
ARM64 with -O2 and -mbranch-protection=standard, cut to the seven functions
the script checks. Each test below edits one function the way a change of the C++
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
import code_shape_mutants  # noqa: E402

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
    return code_shape.check_function(program, function, "writer", {0},
                                     (True, 6))


def insert_after(address, line):
    """Inserts `line` after the instruction at `address` (as printed)."""
    def change(lines):
        index = next(i for i, l in enumerate(lines)
                     if l.strip().startswith(address))
        lines.insert(index + 1, line)
        return lines
    return change


def second_load_from(operand):
    """Makes the first load after DMB ISHLD load `operand` instead."""
    def change(lines):
        fence = next(i for i, l in enumerate(lines)
                     if is_instruction(l, "dmb", "ishld"))
        index = next(i for i in range(fence + 1, len(lines))
                     if code_shape.INSTRUCTION.match(lines[i]) and
                     code_shape.INSTRUCTION.match(lines[i]).group(
                         2).startswith("ldr"))
        lines[index] = re.sub(r"\[[^\]]*\]", operand, lines[index])
        return lines
    return change


def release_store_to(operand):
    """Makes the release store store to `operand`."""
    def change(lines):
        index = next(i for i, l in enumerate(lines)
                     if is_instruction(l, "stlr"))
        lines[index] = re.sub(r"\[[^\]]*\]", operand, lines[index])
        return lines
    return change


def at_entry(line):
    """Inserts `line` after the first instruction of the function."""
    def change(lines):
        index = next(i for i, l in enumerate(lines)
                     if code_shape.INSTRUCTION.match(l))
        lines.insert(index + 1, line)
        return lines
    return change


def second_load_acquire(lines):
    """Makes the first load after DMB ISHLD an acquire load (LDAR)."""
    fence = next(i for i, l in enumerate(lines)
                 if is_instruction(l, "dmb", "ishld"))
    index = next(i for i in range(fence + 1, len(lines))
                 if code_shape.INSTRUCTION.match(lines[i]) and
                 code_shape.INSTRUCTION.match(lines[i]).group(
                     2).startswith("ldr"))
    lines[index] = re.sub(r"\tldr\t(.*), \[([^\]]*)\]$", r"\tldar\t\1, [\2]",
                          lines[index])
    assert "ldar" in lines[index], lines[index]
    return lines


def rename_operand(old, new):
    def change(lines):
        result = [re.sub(rf"\t{old}$", f"\t{new}", l) for l in lines]
        assert result != lines
        return result
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

    def test_a_reader_whose_second_load_loads_something_else_fails(self):
        # Issue #50: the load after the fence must be the second load of
        # the sequence number or of write_seq.
        for function, operand in (("read_record", "[x0, #0x28]"),
                                  ("read_string_record", "[x1]"),
                                  ("read_change", "[x0, #0x40]")):
            with self.subTest(function):
                self.assert_caught(function, second_load_from(operand))

    def test_read_change_with_a_load_of_the_region_after_the_second_load_fails(
            self):
        # Issue #50: a load through the region base, outside the window.
        self.assert_caught("read_change", insert_after(
            "204:", "     206:      \tldr\tx9, [x0, #0x40]"))

    def test_a_reader_that_accepts_after_part_of_the_copy_fails(self):
        # Review of PR #54: a branch to the stores of the result after 4 of
        # the 8 copy loads, so without the fence and the second load.
        self.assert_caught("read_record", insert_after(
            "3c:", "      3e:      \tcbz\tw18, 0x60 <read_record+0x60>"))

    def test_read_change_may_load_the_cursor_before_the_second_load(self):
        # Review of PR #54: a compiler may load the private cursor first.
        def swap(lines):
            first = next(i for i, l in enumerate(lines)
                         if l.strip().startswith("204:"))
            second = next(i for i, l in enumerate(lines)
                          if l.strip().startswith("208:"))
            lines[first], lines[second] = lines[second], lines[first]
            return lines
        text = edit(self.text, "read_change", swap)
        self.assertEqual(code_shape.check(text), [])

    def test_a_missing_function_is_not_a_finding(self):
        findings = []
        code_shape.check(edit(self.text, "read_change", lambda lines: []),
                         findings=findings)
        self.assertEqual(findings,
                         [("read_change", "found 0 definitions", False)])

    def test_read_change_may_load_the_cursor_after_the_second_load(self):
        # The cursor is the caller's, not shared memory.
        text = edit(self.text, "read_change", insert_after(
            "204:", "     206:      \tldr\tx9, [x3]"))
        self.assertEqual(code_shape.check(text), [])

    def test_a_string_read_with_a_slot_load_after_the_second_load_fails(self):
        # Issue #50: any load of the slot after the second load is outside
        # the window, also where the copy loads the slot once (GCC).
        self.assert_caught("read_string_record", insert_after(
            "17c:", "     17e:      \tldrh\tw9, [x1]"))

    def test_a_reader_with_a_shared_load_before_the_acquire_load_fails(self):
        # Issue #63: a load hoisted above the retry loop is outside every
        # attempt, also where it is the first load of a new field.
        for function, operand in (("read_record", "[x0, #0x8]"),
                                  ("read_string_record", "[x1]"),
                                  ("read_change", "[x0, #0x40]")):
            with self.subTest(function):
                failures = code_shape.check(edit(
                    self.text, function,
                    at_entry(f"       2:      \tldr\tx9, {operand}")))
                self.assertTrue(any(
                    f.startswith(f"scada::tag::{function}(") and
                    "before the acquire load" in f for f in failures),
                    failures)

    def test_a_reader_may_load_the_cursor_before_the_acquire_load(self):
        text = edit(self.text, "read_change",
                    at_entry("       2:      \tldr\tx9, [x3]"))
        self.assertEqual(code_shape.check(text), [])

    def test_a_stronger_ordering_in_a_reader_fails(self):
        # Issue #63, kept as D-37 has it: an acquire second load or DMB ISH
        # for fence_acquire is safe, but it is not the code the litmus tests
        # model, so it fails until the tests and the check change with it.
        for function in READERS:
            with self.subTest(function, change="acquire second load"):
                self.assert_caught(function, second_load_acquire)
            with self.subTest(function, change="DMB ISH"):
                self.assert_caught(function, rename_operand("ishld", "ish"))

    def test_a_writer_whose_release_store_misses_the_sequence_fails(self):
        for function in ("write_record", "write_string_record"):
            with self.subTest(function):
                self.assert_caught(function, release_store_to("[x8, #0x4]"))

    def test_a_repair_without_its_release_store_fails(self):
        self.assert_caught("repair_tag_records", rename("stlr", "str"))

    def test_a_repair_without_the_store_of_the_flags_fails(self):
        self.assert_caught("repair_tag_records", drop("strh", "w13,"))

    def test_a_repair_with_a_fence_fails(self):
        self.assert_caught("repair_tag_records", insert_after(
            "d8:", "      da:      \tdmb\tish"))

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


# A writer in the shape GCC gives with -freorder-blocks-and-partition: the
# unlikely path is a part of its own, `[clone .cold]`, in another section,
# reached by a conditional branch with a relocation to that section.
GCC_COLD_WRITER = """\
tag_record.cpp.o:     file format elf64-littleaarch64


Disassembly of section .text:

0000000000000000 <scada::tag::write_record(scada::tag::TagRecord&, int)>:
   0:\tcbz\tx1, 0 <scada::tag::write_record(scada::tag::TagRecord&, int)>
\t\t\t0: R_AARCH64_CONDBR19\t.text.unlikely
   4:\tldr\tw9, [x0]
   8:\tstr\tw10, [x0]
   c:\tdmb\tish
  10:\tstrb\tw1, [x0, #5]
  14:\tstrh\tw2, [x0, #6]
  18:\tstr\tx3, [x0, #8]
  1c:\tstr\tx4, [x0, #16]
  20:\tstr\tx5, [x0, #24]
  24:\tstr\tx6, [x0, #32]
  28:\tstlr\tw9, [x0]
  2c:\tret

Disassembly of section .text.unlikely:

0000000000000000 <scada::tag::write_record(scada::tag::TagRecord&, int) \
[clone .cold]>:
   0:\tstr\tw10, [x0]
   4:\tdmb\tish
   8:\tbl\t0 <scada::tag::write_record(scada::tag::TagRecord&, int)>
\t\t\t8: R_AARCH64_CALL26\tstd::__glibcxx_assert_fail(char const*, int, \
char const*, char const*)
"""

WRITE_RECORD_ONLY = (("scada::tag::write_record(", "writer", {0}, (True, 6)),)


class GccColdTest(unittest.TestCase):

    def test_a_cold_part_that_does_not_return_is_left_out(self):
        self.assertEqual(code_shape.check(GCC_COLD_WRITER,
                                          checks=WRITE_RECORD_ONLY), [])

    def test_a_cold_part_that_jumps_back_is_followed(self):
        # The cold part returns to the hot part after the field stores.
        text = GCC_COLD_WRITER.replace(
            "   8:\tbl\t0 <scada::tag::write_record(scada::tag::TagRecord&, "
            "int)>\n\t\t\t8: R_AARCH64_CALL26\tstd::__glibcxx_assert_fail("
            "char const*, int, char const*, char const*)\n",
            "   8:\tb\t0 <scada::tag::write_record(scada::tag::TagRecord&, "
            "int)>\n\t\t\t8: R_AARCH64_JUMP26\t.text+0x28\n")
        (failure,) = code_shape.check(text, checks=WRITE_RECORD_ONLY)
        self.assertIn("fewer than 6 stores", failure)


COLD_FIXTURE = HERE / "fixtures" / "gcc13-aarch64-O2-cold.txt"
READ_PAIR = (("scada::model_fixture::read_pair(", "reader", {0}, 2),)


class GccColdFixtureTest(unittest.TestCase):
    """Issue #63: a part GCC 13.3 really split off, from
    fixtures/gcc13-cold.cpp: a jump to `.text.unlikely+0x8`, a call of a
    function of that section, and a jump back to `.text+0x4c`."""

    def setUp(self):
        self.text = COLD_FIXTURE.read_text(encoding="utf-8")

    def test_the_fixture_as_compiled_has_the_shape(self):
        self.assertEqual(code_shape.check(self.text, checks=READ_PAIR), [])

    def test_the_cold_part_is_one_definition_with_its_function(self):
        program = code_shape.Program(self.text)
        self.assertEqual(len(program.find(READ_PAIR[0][0])), 2)
        self.assertEqual(len([f for f in program.find(READ_PAIR[0][0])
                              if "[clone" not in f.name]), 1)

    def test_a_load_in_the_cold_part_is_checked_with_the_registers(self):
        # x19 holds the record in the cold part too: a load of it there,
        # before the attempt that went there retries, is a load without the
        # fence.
        text = self.text.replace(
            "  14:\tldp\tx1, x2, [sp, #48]\n",
            "  14:\tldp\tx1, x2, [sp, #48]\n  16:\tldr\tx9, [x19, #8]\n")
        self.assertNotEqual(text, self.text)
        (failure,) = code_shape.check(text, checks=READ_PAIR)
        self.assertIn("ends without DMB ISHLD", failure)

    def test_a_cold_part_that_jumps_past_the_check_fails(self):
        # Back to the second load's comparison instead of the retry: the
        # attempt reads its second load without the copy before the fence.
        text = self.text.replace("R_AARCH64_JUMP26\t.text+0x4c",
                                 "R_AARCH64_JUMP26\t.text+0x3c")
        self.assertNotEqual(text, self.text)
        (failure,) = code_shape.check(text, checks=READ_PAIR)
        self.assertIn("fewer than 2 loads", failure)


class FrameTest(unittest.TestCase):
    """Accesses of the function's own frame are not counted (issue #50)."""

    def check(self, extra):
        text = GCC_STYLE_WRITER.replace(
            "      30:\tstlr\tw9, [x0]\n",
            "      30:\tstlr\tw9, [x0]\n" + extra)
        return check_gcc_style_writer(text)

    def test_a_store_through_the_frame_pointer_is_not_counted(self):
        self.assertIsNone(self.check("      31:\tstr\tx1, [x29, #16]\n"))

    def test_a_store_through_a_copy_of_the_stack_pointer_is_not_counted(self):
        self.assertIsNone(self.check("      31:\tadd\tx9, sp, #0x10\n"
                                     "      32:\tstr\tx1, [x9]\n"))

    def test_a_store_of_the_record_after_the_release_store_fails(self):
        self.assertIn("a store after the release store",
                      self.check("      31:\tstr\tx1, [x0, #16]\n"))


class MutantCommandTest(unittest.TestCase):
    """The command that compiles an edited source (issue #63)."""

    SOURCE = pathlib.Path("/src/libs/scada-tag/src/tag_record.cpp")

    def command(self, arguments):
        return code_shape_mutants.mutant_command(
            arguments, self.SOURCE, pathlib.Path("/tmp/w/tag_record.cpp"),
            pathlib.Path("/tmp/w/tag_record.o"))

    def test_a_compiler_launcher_stays_first(self):
        command = self.command(["ccache", "g++", "-O2", "-o", "x.o", "-c",
                                str(self.SOURCE)])
        self.assertEqual(command[:3], ["ccache", "g++", "-O2"])
        self.assertEqual(command[3:], [
            "-iquote", "/src/libs/scada-tag/src", "-Wno-error",
            "-o", "/tmp/w/tag_record.o", "-c", "/tmp/w/tag_record.cpp"])

    def test_no_file_of_the_build_is_written(self):
        command = self.command([
            "g++", "-MD", "-MT", "obj.o", "-MF", "obj.o.d", "-MMD", "-MP",
            "-MQ", "q", "-MFjoined.d", "-O2", "-o", "obj.o", "-c",
            str(self.SOURCE)])
        self.assertEqual(command[:2], ["g++", "-O2"])
        for flag in ("-MD", "-MMD", "-MP", "-MT", "-MF", "-MQ", "obj.o",
                     "obj.o.d", "q", "-MFjoined.d", str(self.SOURCE)):
            self.assertNotIn(flag, command)
        self.assertEqual(command.count("-o"), 1)
        self.assertEqual(command.count("-c"), 1)


if __name__ == "__main__":
    unittest.main()
