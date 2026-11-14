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
            result.append([header.group(1), []])
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
    if not match or match.group(1) != mnemonic:
        return False
    return operand is None or match.group(2).split()[0] == operand


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

    def test_a_call_that_is_not_panic_fails(self):
        def add_call(lines):
            index = next(i for i, l in enumerate(lines)
                         if is_instruction(l, "dmb", "ish"))
            lines.insert(index + 1, "     100:      \tbl\t0x0 <x>")
            lines.insert(index + 2,
                         "\t\t0000000000000100:  R_AARCH64_CALL26\tother()")
            return lines
        self.assert_caught("write_record", add_call)


if __name__ == "__main__":
    unittest.main()
