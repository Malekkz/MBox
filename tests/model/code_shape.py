#!/usr/bin/env python3
"""Checks that the ARM64 library compiles to the order the litmus tests model.

seqlock.litmus and change-ring.litmus model the instructions the protocol
functions compile to. This script disassembles those functions from the
tag library and checks the order of their memory accesses and barriers, so
that an edit that drops, weakens or moves a fence or an acquire or release
access fails here although the model still passes. See README.md.

The order is checked along every path through a function from its entry to
a return, with each branch taken at most twice, so a retry loop runs one or
two times. Calls of functions of the same object (helpers the compiler did
not inline) are followed into them. A path that ends in a function that does
not return (`panic`, a failed library assertion, the stack protector) is
left out: it publishes nothing.

Input: the disassembly of `objdump -d -r -C --no-show-raw-insn` (GNU or
LLVM) of an ARM64 object or static library, from a build with optimisation.
"""

import argparse
import re
import subprocess
import sys

OBJECT = re.compile(r"^(\S+):\s+file format")
FUNCTION = re.compile(r"^([0-9a-f]+) <(.+)>:\s*$")
INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+([a-z][a-z0-9.]*)\s*(.*)$")
RELOCATION = re.compile(
    r"^\s*[0-9a-f]+:\s+R_AARCH64_(?:CALL|JUMP)26\s+(.+?)\s*$")
TARGET = re.compile(r"(?:0x)?([0-9a-f]+)\s+<([^>]*)>")

# Functions that do not return. A path that calls one ends there.
NO_RETURN = ("scada::panic(", "std::__glibcxx_assert_fail(",
             "__stack_chk_fail", "abort")

BRANCHES = ("b", "cbz", "cbnz", "tbz", "tbnz")
CALLS = ("bl",)
RETURNS = ("ret", "retaa", "retab")

# Path limits: more is a defect of this script or a very different compiler.
MAX_PATHS = 20000
MAX_DEPTH = 6

# One letter for each instruction that matters to the order:
#   A  acquire load (LDAR, LDAPR)     R  release store (STLR)
#   F  DMB ISH (fence_release)        G  DMB ISHLD (fence_acquire)
#   L  other load                     S  other store
#   C  call of a function not found   X  any other barrier or atomic
# Accesses through the stack pointer are the function's own frame and are
# left out.


def classify(mnemonic, operands):
    if mnemonic.startswith(("ldar", "ldapr")):
        return "A"
    if mnemonic.startswith("stlr"):
        return "R"
    if mnemonic == "dmb":
        option = operands.split()[0] if operands else ""
        return {"ish": "F", "ishld": "G"}.get(option, "X")
    if mnemonic == "dsb" or mnemonic.startswith(
            ("ldadd", "swp", "cas", "ldxr", "stxr", "ldaxr", "stlxr",
             "ldaxp", "stlxp")):
        return "X"
    on_stack = "[sp" in operands.replace(" ", "")
    if mnemonic.startswith(("ldr", "ldur", "ldp")) and not on_stack:
        return "L"
    if mnemonic.startswith(("str", "stur", "stp")) and not on_stack:
        return "S"
    return None


class Instruction:
    def __init__(self, address, mnemonic, operands):
        self.address = address
        self.mnemonic = mnemonic
        self.operands = operands
        self.letter = classify(mnemonic, operands)
        self.symbol = None  # target of a call or a jump out of the function
        self.target = None  # address of a branch
        match = TARGET.search(operands)
        if match:
            self.target = int(match.group(1), 16)
            self.symbol = match.group(2)

    def text(self):
        return f"{self.mnemonic} {self.operands}".strip()


class Function:
    def __init__(self, obj, name, lines):
        self.obj = obj
        self.name = name
        self.lines = lines  # the disassembly as printed, for reports
        self.code = []

    def index_of(self, address):
        for index, instruction in enumerate(self.code):
            if instruction.address == address:
                return index
        return None


def parse(disassembly):
    """Returns the list of functions of the disassembly."""
    result = []
    obj = ""
    current = None
    for line in disassembly.splitlines():
        found = OBJECT.match(line)
        if found:
            obj = found.group(1)
            current = None
            continue
        header = FUNCTION.match(line)
        if header:
            current = Function(obj, header.group(2), [line])
            result.append(current)
            continue
        if current is None:
            continue
        current.lines.append(line)
        relocation = RELOCATION.match(line)
        if relocation and current.code:
            # In an object file the operand of a call or a jump to another
            # function is a placeholder; the relocation names the target.
            current.code[-1].symbol = relocation.group(1)
            current.code[-1].target = None
            continue
        instruction = INSTRUCTION.match(line)
        if instruction:
            current.code.append(Instruction(int(instruction.group(1), 16),
                                            instruction.group(2),
                                            instruction.group(3)))
    return result


class Program:
    def __init__(self, disassembly):
        self.functions = parse(disassembly)
        self.cache = {}

    def find(self, prefix):
        return [f for f in self.functions if f.name.startswith(prefix)]

    def callee(self, caller, symbol):
        """The function a call names: in the caller's object if it has one
        of that name, else the only one in the input."""
        name = re.sub(r"\+0x[0-9a-f]+$", "", symbol or "")
        same = [f for f in self.functions
                if f.name == name and f.obj == caller.obj]
        if len(same) == 1:
            return same[0]
        anywhere = [f for f in self.functions if f.name == name]
        return anywhere[0] if len(anywhere) == 1 else None

    def paths(self, function, depth=0):
        """Every path from the entry of `function` to a return, as a tuple
        of (letter, text) pairs, each branch taken at most twice."""
        key = (function.obj, function.name)
        if key in self.cache:
            return self.cache[key]
        if depth > MAX_DEPTH:
            raise RuntimeError(f"calls nested too deep in {function.name}")
        found = []
        # (index, tokens, times each branch was taken)
        stack = [(0, (), {})]
        while stack:
            index, tokens, taken = stack.pop()
            if index is None or index >= len(function.code):
                raise RuntimeError(
                    f"{function.name}: a path leaves the function")
            instruction = function.code[index]
            mnemonic = instruction.mnemonic
            base = mnemonic.split(".")[0]
            if instruction.letter:
                tokens += ((instruction.letter, instruction.text()),)
            if mnemonic in RETURNS:
                found.append(tokens)
            elif mnemonic in CALLS or (base == "b" and instruction.target
                                       is None):
                symbol = instruction.symbol or ""
                if symbol.startswith(NO_RETURN):
                    continue
                callee = self.callee(function, symbol)
                if callee is None:
                    tokens += (("C", f"call {symbol}"),)
                    callee_paths = [()]
                else:
                    callee_paths = self.paths(callee, depth + 1)
                for inner in callee_paths:
                    if base == "b":  # a tail call returns for this function
                        found.append(tokens + inner)
                    else:
                        stack.append((index + 1, tokens + inner, taken))
            elif base in BRANCHES:
                target = function.index_of(instruction.target)
                if target is None:
                    raise RuntimeError(
                        f"{function.name}: branch to an unknown address: "
                        f"{instruction.text()}")
                edge = (index, target)
                if taken.get(edge, 0) < 2:
                    again = dict(taken)
                    again[edge] = taken.get(edge, 0) + 1
                    stack.append((target, tokens, again))
                if mnemonic != "b":  # conditional: may fall through
                    stack.append((index + 1, tokens, taken))
            elif mnemonic in ("br", "blr", "braa", "brab", "blraa", "blrab"):
                tokens += (("C", f"indirect {instruction.text()}"),)
                stack.append((index + 1, tokens, taken))
            else:
                stack.append((index + 1, tokens, taken))
            if len(found) > MAX_PATHS:
                raise RuntimeError(f"{function.name}: too many paths")
        self.cache[key] = found
        return found


def letters_of(path):
    return "".join(letter for letter, _ in path)


def check_writer(path, store_before_fence, min_stores):
    """One path of the write side: sections 6.1 and 8.1. `min_stores`: the
    fewest stores the fields can take; compilers do not merge atomic
    stores."""
    letters = letters_of(path)
    for bad in "AGXC":
        if bad in letters:
            return f"unexpected instruction ({bad})"
    if letters.count("F") != 1:
        return f"expected one DMB ISH, found {letters.count('F')}"
    if letters.count("R") != 1:
        return f"expected one release store (STLR), found {letters.count('R')}"
    fence = letters.index("F")
    release = letters.index("R")
    if release < fence:
        return "the release store comes before the fence"
    if letters[fence:release].count("S") < min_stores:
        return (f"fewer than {min_stores} stores between the fence and the "
                "release store")
    if "S" in letters[release:]:
        return "a store after the release store"
    before = letters[:fence]
    if store_before_fence and "S" not in before:
        return "no store before the fence (the odd sequence number)"
    if not store_before_fence and "S" in before:
        return "a store before the fence (an entry store not ordered)"
    return None


def check_reader(path, min_loads):
    """One path of the read side: sections 6.2 and 8.2. Each attempt starts
    with the acquire load; one that uses its copy has the fence after at
    least `min_loads` loads (compilers do not merge atomic loads) and a load
    after the fence. Returns (problem, whether an attempt had the fence)."""
    letters = letters_of(path)
    for bad in "FRXC":
        if bad in letters:
            return f"unexpected instruction ({bad})", False
    attempts = letters.split("A")
    if "G" in attempts[0]:
        return "a fence before the acquire load", False
    fenced = False
    for attempt in attempts[1:]:
        if attempt.count("G") > 1:
            return "two fences in one attempt", False
        if "G" in attempt:
            fenced = True
            copy, after = attempt.split("G")
            if copy.count("L") < min_loads:
                return (f"fewer than {min_loads} loads between the acquire "
                        "load and the fence"), False
            if "L" not in after:
                return "no load after the fence (the second load)", False
    return None, fenced


def check_function(program, function, rule, argument):
    paths = program.paths(function)
    if not paths:
        return "no path returns"
    if rule == "writer":
        store_before_fence, min_stores = argument
        for path in paths:
            problem = check_writer(path, store_before_fence, min_stores)
            if problem:
                return f"{problem}\n    path: {shape(path)}"
        return None
    any_fenced = False
    for path in paths:
        problem, fenced = check_reader(path, argument)
        if problem:
            return f"{problem}\n    path: {shape(path)}"
        any_fenced = any_fenced or fenced
    if not any_fenced:
        return "no path has an acquire load followed by DMB ISHLD"
    return None


def shape(path):
    return " | ".join(text for _, text in path)


# Function name prefix (as objdump -C prints it), its side and the counts:
# a write stores 6 or 7 of the 8 fields of TagRecord other than `seq`, a
# string write also `len` (the bytes are none for an empty string), an
# entry has 7 fields; a
# read copies the 8 fields, a string read also `len` and the bytes (a loop:
# at least 2 loads), an entry read its 7 fields.
CHECKS = (
    ("scada::tag::write_record(", "writer", (True, 6)),
    ("scada::tag::write_string_record(", "writer", (True, 7)),
    ("scada::tag::read_record(", "reader", 8),
    ("scada::tag::read_string_record(", "reader", 10),
    ("scada::tag::append_change(", "writer", (False, 7)),
    ("scada::tag::read_change(", "reader", 7),
)


def check(disassembly, report=None):
    """Returns the list of failures; empty if the code has the shape. With
    `report`, the disassembly of each function that fails, and of the
    functions it calls, is appended to that list."""
    program = Program(disassembly)
    failures = []
    for prefix, rule, argument in CHECKS:
        functions = program.find(prefix)
        if len(functions) != 1:
            failures.append(f"{prefix}...): found {len(functions)} "
                            "definitions")
            continue
        try:
            problem = check_function(program, functions[0], rule, argument)
        except RuntimeError as error:
            problem = str(error)
        if problem:
            failures.append(f"{functions[0].name}: {problem}")
            if report is not None:
                report.extend(related(program, functions[0]))
    return failures


def related(program, function, seen=None):
    """The disassembly of `function` and of the functions it calls."""
    seen = set() if seen is None else seen
    if (function.obj, function.name) in seen:
        return []
    seen.add((function.obj, function.name))
    lines = list(function.lines)
    for instruction in function.code:
        if instruction.symbol and not instruction.symbol.startswith(
                NO_RETURN):
            callee = program.callee(function, instruction.symbol)
            if callee is not None and callee is not function:
                lines += related(program, callee, seen)
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump", required=True, help="path of objdump")
    parser.add_argument("library", help="the tag library (libscada-tag.a)")
    args = parser.parse_args()
    disassembly = subprocess.run(
        [args.objdump, "-d", "-r", "-C", "--no-show-raw-insn", args.library],
        capture_output=True, text=True, check=True).stdout
    report = []
    failures = check(disassembly, report)
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        # The code that failed, so that it can be read from the job log.
        print("\nDisassembly of the functions that failed:")
        print("\n".join(report))
        return 1
    print(f"ok   {len(CHECKS)} functions have the order of the model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
