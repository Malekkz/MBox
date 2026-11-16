#!/usr/bin/env python3
"""Checks that the ARM64 library compiles to the order the litmus tests model.

seqlock.litmus, change-ring.litmus and repair.litmus model the instructions
the protocol functions compile to. This script disassembles those functions
from the tag library and checks the order of their memory accesses and
barriers, so that an edit that drops, weakens or moves a fence or an acquire
or release access fails here although the model still passes. See README.md.

The order is checked along every path through a function from its entry to
a return, with each branch taken at most twice, so a retry loop runs one or
two times. Calls of functions of the same object (helpers the compiler did
not inline) are followed into them; a jump into a part the compiler split
off (GCC's `[clone .cold]`) and back continues the same walk. A path that
ends in a function that does not return (`panic`, a failed library
assertion, the stack protector) is left out: it publishes nothing.

Along each path the registers and the stack slots are followed
symbolically from the entry of the function, so each memory access is known
as an offset from an argument, from the stack pointer, or as unknown. The
arguments that point at shared memory are listed for each function
(CHECKS); an access through any other argument is private, one through the
stack pointer (also through the frame pointer x29 or a copy of either) is
the function's own frame and is left out.

Input: the disassembly of `objdump -d -r -C --no-show-raw-insn` (GNU or
LLVM) of an ARM64 object or static library, from a build with optimisation.
"""

import argparse
import itertools
import re
import subprocess
import sys

OBJECT = re.compile(r"^(\S+):\s+file format")
SECTION = re.compile(r"^Disassembly of section (\S+):\s*$")
FUNCTION = re.compile(r"^([0-9a-f]+) <(.+)>:\s*$")
INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+([a-z][a-z0-9.]*)\s*(.*)$")
RELOCATION = re.compile(
    r"^\s*[0-9a-f]+:\s+R_AARCH64_(?:CALL26|JUMP26|CONDBR19|TSTBR14)\s+"
    r"(.+?)\s*$")
TARGET = re.compile(r"(?:0x)?([0-9a-f]+)\s+<([^>]*)>")
REGISTER = re.compile(r"^([xw])(\d+|zr)$|^(sp|wsp)$")
MEMORY = re.compile(r"\[([^\]]*)\](!?)(?:\s*,\s*#(-?(?:0x)?[0-9a-f]+))?")
IMMEDIATE = re.compile(r"^#(-?(?:0x)?[0-9a-f]+)$")
OFFSET_SYMBOL = re.compile(r"^(.*?)\+0x([0-9a-f]+)$")

# Functions that do not return. A path that calls one ends there.
NO_RETURN = ("scada::panic(", "std::__glibcxx_assert_fail(",
             "__stack_chk_fail", "abort")

BRANCHES = ("b", "cbz", "cbnz", "tbz", "tbnz")
CALLS = ("bl",)
RETURNS = ("ret", "retaa", "retab")
INDIRECT = ("br", "blr", "braa", "brab", "blraa", "blrab")

# Instructions whose first operand is not a register they write.
NO_DESTINATION = ("cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fccmp",
                  "prfm", "nop", "hint", "bti", "isb", "dmb", "dsb", "ret",
                  "paciasp", "autiasp", "pacibsp", "autibsp")

# Path limits: more is a defect of this script or a very different compiler.
MAX_PATHS = 20000
MAX_DEPTH = 6

# Bytes of a load or store, by the register it moves.
WIDTH = {"x": 8, "w": 4, "q": 16, "d": 8, "s": 4, "h": 2, "b": 1}

# One letter for each instruction that matters to the order:
#   A  acquire load (LDAR, LDAPR)     R  release store (STLR)
#   F  DMB ISH (fence_release)        G  DMB ISHLD (fence_acquire)
#   L  other load                     S  other store
#   C  call of a function not found   X  any other barrier or atomic


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
    if mnemonic.startswith(("ldr", "ldur", "ldp")) and "[" in operands:
        return "L"
    if mnemonic.startswith(("str", "stur", "stp")) and "[" in operands:
        return "S"
    return None


class Instruction:
    def __init__(self, address, mnemonic, operands):
        self.address = address
        self.mnemonic = mnemonic
        self.operands = operands.split("//")[0].strip()
        self.letter = classify(mnemonic, self.operands)
        self.symbol = None  # target of a call or a jump out of the function
        self.target = None  # address of a branch
        match = TARGET.search(operands)
        if match:
            self.target = int(match.group(1), 16)
            self.symbol = match.group(2)

    def text(self):
        return f"{self.mnemonic} {self.operands}".strip()


class Function:
    def __init__(self, obj, section, start, name, lines):
        self.obj = obj
        self.section = section
        self.start = start
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
    section = ""
    current = None
    for line in disassembly.splitlines():
        found = OBJECT.match(line)
        if found:
            obj = found.group(1)
            current = None
            continue
        found = SECTION.match(line)
        if found:
            section = found.group(1)
            current = None
            continue
        header = FUNCTION.match(line)
        if header:
            current = Function(obj, section, int(header.group(1), 16),
                               header.group(2), [line])
            result.append(current)
            continue
        if current is None:
            continue
        current.lines.append(line)
        relocation = RELOCATION.match(line)
        if relocation and current.code:
            # In an object file the operand of a call or a jump to another
            # function or section is a placeholder; the relocation names
            # the target.
            current.code[-1].symbol = relocation.group(1)
            current.code[-1].target = None
            continue
        instruction = INSTRUCTION.match(line)
        if instruction:
            current.code.append(Instruction(int(instruction.group(1), 16),
                                            instruction.group(2),
                                            instruction.group(3)))
    return result


# Events of a path. A path is a tuple of instructions as executed, with two
# markers: CALL_NOT_FOUND (a call of a function not in the input) and
# CLOBBER (the registers a call may change are unknown afterwards).
class StructuralError(RuntimeError):
    """The disassembly cannot be walked: not a finding about the order."""


class Marker:
    def __init__(self, kind, text):
        self.kind = kind
        self.text_ = text

    def text(self):
        return self.text_


class Program:
    def __init__(self, disassembly):
        self.functions = parse(disassembly)
        self.cache = {}

    def find(self, prefix):
        return [f for f in self.functions if f.name.startswith(prefix)]

    def resolve(self, caller, symbol):
        """The function and the instruction index a call or a jump names:
        a function by name (in the caller's object if it has one of that
        name, else the only one in the input), or a section of the caller's
        object with an offset, as relocations to a split-off part do.
        Returns None if not found."""
        name, offset = symbol or "", 0
        match = OFFSET_SYMBOL.match(name)
        if match:
            name, offset = match.group(1), int(match.group(2), 16)
        same = [f for f in self.functions
                if f.name == name and f.obj == caller.obj]
        if len(same) != 1:
            same = [f for f in self.functions if f.name == name]
        if len(same) == 1:
            index = same[0].index_of(same[0].start + offset)
            return None if index is None else (same[0], index)
        # A section symbol: the function of that section holding the offset.
        in_section = [f for f in self.functions
                      if f.obj == caller.obj and f.section == name]
        for function in in_section:
            index = function.index_of(offset)
            if index is not None:
                return function, index
        return None

    def paths(self, function, start=0, depth=0, times=2):
        """Every path from instruction `start` of `function` to a return,
        as a tuple of instructions and markers, each branch taken at most
        `times` times.

        A jump between a function and a part split off from it (`[clone
        .cold]`), either way, continues the same walk, with the same counts
        of branches taken: GCC jumps back from such a part into the loop it
        left (issue #63), and walking the part as a call of its own would
        start the counts again at each visit."""
        key = (function.obj, function.section, function.name, start, times)
        if key in self.cache:
            return self.cache[key]
        if depth > MAX_DEPTH:
            raise StructuralError(f"calls nested too deep in {function.name}")
        found = []
        # (function or part of it, index, events, times each branch was
        # taken)
        stack = [(function, start, (), {})]
        while stack:
            part, index, events, taken = stack.pop()
            if index is None or index >= len(part.code):
                raise StructuralError(
                    f"{part.name}: a path leaves the function")
            instruction = part.code[index]
            mnemonic = instruction.mnemonic
            base = mnemonic.split(".")[0]
            events += (instruction,)
            external = instruction.target is None and instruction.symbol
            conditional = base in BRANCHES and mnemonic != "b"

            def branch(target_part, target_index):
                edge = (id(part), index, id(target_part), target_index)
                if taken.get(edge, 0) < times:
                    again = dict(taken)
                    again[edge] = taken.get(edge, 0) + 1
                    stack.append((target_part, target_index, events, again))

            if mnemonic in RETURNS:
                found.append(events)
            elif mnemonic in CALLS or (base in BRANCHES and external):
                symbol = instruction.symbol or ""
                if symbol.startswith(NO_RETURN):
                    if mnemonic not in CALLS and mnemonic != "b":
                        stack.append((part, index + 1, events, taken))
                    continue
                target = self.resolve(part, symbol)
                if mnemonic not in CALLS and target is not None and \
                        same_function(part, target[0]):
                    # Into the split-off part, or back from it.
                    branch(*target)
                    if conditional:
                        stack.append((part, index + 1, events, taken))
                    continue
                if target is None:
                    inner_paths = [(Marker("C", f"call {symbol}"),
                                    Marker("clobber", "(call returns)"))]
                else:
                    inner_paths = self.paths(target[0], target[1], depth + 1,
                                             times)
                if mnemonic in CALLS:
                    for inner in inner_paths:
                        stack.append((part, index + 1, events + inner,
                                      taken))
                else:
                    # A tail call: its paths end this one. A conditional
                    # jump may also fall through.
                    for inner in inner_paths:
                        found.append(events + inner)
                    if conditional:
                        stack.append((part, index + 1, events, taken))
            elif base in BRANCHES:
                target = part.index_of(instruction.target)
                if target is None and mnemonic == "b" and self.resolve(
                        part, instruction.symbol) is not None:
                    # A tail call within the section, without relocation.
                    callee, start_index = self.resolve(part,
                                                       instruction.symbol)
                    for inner in self.paths(callee, start_index, depth + 1,
                                            times):
                        found.append(events + inner)
                    continue
                if target is None:
                    raise StructuralError(
                        f"{part.name}: branch to an unknown address: "
                        f"{instruction.text()}")
                branch(part, target)
                if conditional:
                    stack.append((part, index + 1, events, taken))
            elif mnemonic in INDIRECT:
                stack.append((part, index + 1, events + (
                    Marker("C", f"indirect {instruction.text()}"),
                    Marker("clobber", "(call returns)")), taken))
            else:
                stack.append((part, index + 1, events, taken))
            if len(found) > MAX_PATHS:
                raise StructuralError(f"{function.name}: too many paths")
        self.cache[key] = found
        return found


COLD = re.compile(r" \[clone \.cold(?:\.\d+)?\]$")


def same_function(a, b):
    """Whether `a` and `b` are one function or parts of one: a function and
    the part the compiler split off from it (`f(...) [clone .cold]`)."""
    return a.obj == b.obj and COLD.sub("", a.name) == COLD.sub("", b.name)


# Symbolic values. A value is (root, offset): root is ("arg", n) for the
# value argument register xn had at the entry, ("sp",) for the stack
# pointer at the entry, ("private", id) for an address of memory the
# protocol does not share (a global, something loaded from one), or
# ("unknown", id) for anything else; offset is a number of bytes, or None
# when an index was added.
SP = ("sp",)
_ids = itertools.count()


def fresh(kind="unknown"):
    return ((kind, next(_ids)), 0)


def parse_register(text):
    """("x", n) for a general register, ("sp",) for the stack pointer,
    ("zr",) for the zero register, None otherwise."""
    match = REGISTER.match(text.strip())
    if not match:
        return None
    if match.group(3):
        return ("sp",)
    if match.group(2) == "zr":
        return ("zr",)
    return ("x", int(match.group(2)), match.group(1))


def parse_immediate(text):
    match = IMMEDIATE.match(text.strip())
    return int(match.group(1), 0) if match else None


class Machine:
    """The registers and the 8-byte stack slots along one path."""

    def __init__(self):
        self.registers = {n: ((("arg", n)), 0) for n in range(9)}
        for n in range(9, 31):
            self.registers[n] = fresh()
        self.registers[29] = (SP, None)  # the caller's frame
        self.sp = (SP, 0)
        self.slots = {}

    def get(self, register):
        if register is None:
            return fresh()
        if register[0] == "sp":
            return self.sp
        if register[0] == "zr":
            return (("const",), 0)
        if register[2] == "w":
            return fresh("int")  # 32 bits do not hold an address
        return self.registers[register[1]]

    def set(self, register, value):
        if register is None or register[0] == "zr":
            return
        if register[0] == "sp":
            self.sp = value
        elif register[2] == "w":
            self.registers[register[1]] = fresh("int")
        else:
            self.registers[register[1]] = value

    def clobber(self):
        for n in range(19):
            self.registers[n] = fresh()

    def address(self, operands):
        """The address of a memory operand, and the register and value a
        writeback leaves in its base."""
        match = MEMORY.search(operands)
        parts = [p.strip() for p in match.group(1).split(",")]
        base_register = parse_register(parts[0])
        root, offset = self.get(base_register)
        if len(parts) > 1:
            immediate = parse_immediate(parts[1])
            if immediate is None:
                offset = None  # an index register
            elif offset is not None:
                offset += immediate
        post = parse_immediate("#" + match.group(3)) if match.group(3) else 0
        writeback = None
        if match.group(2) or match.group(3):
            base_root, base_offset = self.get(base_register)
            final = (root, offset if match.group(2) else
                     (None if base_offset is None else base_offset + post))
            writeback = (base_register, final)
            if match.group(3):  # post-index: the access uses the old base
                root, offset = base_root, base_offset
        return (root, offset), writeback

    def step(self, instruction):
        """Executes one instruction. Returns the address of its memory
        access, or None."""
        mnemonic = instruction.mnemonic
        operands = instruction.operands
        fields = [f.strip() for f in operands.split("[")[0].split(",")
                  if f.strip()]
        if "[" in operands and (instruction.letter in tuple("ALSRX")
                                or mnemonic.startswith("prfm")):
            address, writeback = self.address(operands)
            width = WIDTH.get(fields[0][0], 8) if fields else 8
            pair = mnemonic.startswith(("ldp", "stp", "ldaxp", "stlxp"))
            if mnemonic.endswith("b") and not mnemonic.startswith("ldp"):
                width = 1
            elif mnemonic.endswith("h") and not mnemonic.startswith("ldp"):
                width = 2
            registers = fields[:2] if pair else fields[:1]
            if mnemonic.startswith(("ld", "swp", "cas")):
                for index, field in enumerate(registers):
                    self.set(parse_register(field),
                             self.load(address, index * width, width))
            elif mnemonic.startswith("st"):
                for index, field in enumerate(registers):
                    self.store(address, index * width, width,
                               parse_register(field))
            if writeback:
                self.set(*writeback)
            return address
        if mnemonic in ("mov", "add", "sub") and len(fields) >= 2:
            destination = parse_register(fields[0])
            source = parse_register(fields[1])
            if mnemonic == "mov":
                immediate = parse_immediate(fields[1])
                self.set(destination, (("const",), immediate)
                         if immediate is not None else self.get(source))
                return None
            immediate = parse_immediate(fields[2]) if len(fields) > 2 else None
            if immediate is not None and len(fields) > 3 and "lsl #12" in \
                    fields[3]:
                immediate <<= 12
            root, offset = self.get(source)
            if immediate is not None:
                sign = 1 if mnemonic == "add" else -1
                self.set(destination, (root, None if offset is None
                                       else offset + sign * immediate))
            elif mnemonic == "add" and len(fields) > 2:
                # An index added to an address keeps the address's root.
                other = self.get(parse_register(fields[2]))
                pointers = [v for v in ((root, offset), other)
                            if v[0][0] in ("arg", "sp", "private")]
                self.set(destination, (pointers[0][0], None)
                         if len(pointers) == 1 else fresh())
            else:
                self.set(destination, fresh())
            return None
        if mnemonic in ("adrp", "adr"):
            self.set(parse_register(fields[0]), fresh("private"))
            return None
        if fields and not mnemonic.startswith(NO_DESTINATION) and \
                mnemonic.split(".")[0] not in BRANCHES + CALLS + INDIRECT:
            self.set(parse_register(fields[0]), fresh("int"))
        return None

    def load(self, address, offset, width):
        root, start = address
        if root == SP and start is not None and width == 8:
            return self.slots.get(start + offset, fresh())
        if root[0] == "private":
            return fresh("private")  # e.g. the stack guard through the GOT
        return fresh()

    def store(self, address, offset, width, register):
        root, start = address
        if root != SP:
            return
        if start is None:
            self.slots.clear()
            return
        low = start + offset
        for slot in list(self.slots):
            if slot < low + width and low < slot + 8:
                del self.slots[slot]
        if width == 8 and register is not None:
            self.slots[low] = self.get(register)


def evaluate(path, shared):
    """The events of a path that matter to the order, as tuples
    (letter, text, address, where): `where` is "shared", "private",
    "stack" or "unknown" for an access, None otherwise. Accesses through
    the stack are left out. `shared` is the set of argument registers that
    point at shared memory."""
    machine = Machine()
    events = []
    for item in path:
        if isinstance(item, Marker):
            if item.kind == "clobber":
                machine.clobber()
            else:
                events.append((item.kind, item.text(), None, None))
            continue
        address = machine.step(item)
        if item.letter is None:
            continue
        where = None
        if address is not None:
            kind = address[0][0]
            if kind == "sp":
                continue
            if kind == "arg":
                where = "shared" if address[0][1] in shared else "private"
            elif kind == "private":
                where = "private"
            else:
                where = "unknown"
        events.append((item.letter, item.text(), address, where))
    return events


def letters_of(events):
    return "".join(e[0] for e in events)


def check_writer(events, store_before_fence, min_stores):
    """One path of the write side: sections 6.1 and 8.1. `min_stores`: the
    fewest stores the fields can take; compilers do not merge atomic
    stores."""
    events = [e for e in events if e[0] != "L"]
    letters = letters_of(events)
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
    stores_before = letters[:fence].count("S")
    if store_before_fence:
        if stores_before != 1:
            return (f"expected one store before the fence (the odd sequence "
                    f"number), found {stores_before}")
        odd = events[letters.index("S")][2]
        if odd != events[release][2] or odd[1] is None:
            return ("the store before the fence and the release store do "
                    "not go to the same address (the sequence number)")
    elif stores_before:
        return "a store before the fence (an entry store not ordered)"
    return None


def check_reader(events, min_loads):
    """One path of the read side: sections 6.2 and 8.2. Returns (problem,
    whether an attempt had the fence).

    Each attempt starts with the acquire load of shared memory; no load of
    shared memory comes before the first one. An attempt
    that loads shared memory at all must reach DMB ISHLD after at least
    `min_loads` such loads (a copy; compilers do not merge atomic loads),
    and the first load of other than the caller's own memory after it must
    load the address of the acquire load: the second load. After the second
    load the attempt loads no more shared memory, nor anything this script
    cannot place: that would be a load outside the window. A stronger
    ordering than the litmus tests model (an acquire second load, DMB ISH)
    fails too: the code would no longer be what the model checks."""
    letters = letters_of(events)
    for bad in "FRXC":
        if bad in letters:
            return f"unexpected instruction ({bad})", False
    fenced_any = False
    attempt = None  # the state of the current attempt

    def finish(state):
        if state is None:
            return None
        # Any copy load without the fence fails, not only a whole copy: a
        # branch to the result after part of the copy is a mixed copy.
        if not state["fenced"] and state["loads"] > 0:
            return (f"an attempt makes {state['loads']} loads of shared "
                    "memory and ends without DMB ISHLD")
        if state["fenced"] and state["second"] is None:
            return "no load after the fence (the second load)"
        return None

    for letter, _, address, where in events:
        if letter == "A":
            problem = finish(attempt)
            if problem:
                return problem, False
            if where != "shared" or address[1] is None:
                return ("the acquire load does not load a known place of "
                        "shared memory"), False
            attempt = {"loads": 0, "acquire": address, "fenced": False,
                       "second": None}
        elif letter == "G":
            if attempt is None:
                return "a fence before the acquire load", False
            if attempt["fenced"]:
                return "two fences in one attempt", False
            if attempt["loads"] < min_loads:
                return (f"fewer than {min_loads} loads of shared memory "
                        "between the acquire load and the fence"), False
            attempt["fenced"] = True
            fenced_any = True
        elif letter == "L" and attempt is None:
            # A load the compiler hoisted above the retry loop is outside
            # every attempt's window (issue #63).
            if where == "shared":
                return ("a load of shared memory before the acquire load"
                        ), False
        elif letter == "L":
            if not attempt["fenced"]:
                if where == "shared":
                    attempt["loads"] += 1
            elif attempt["second"] is None:
                if where == "private":
                    # The caller's own memory (the cursor) may be loaded
                    # before the second load; it is not part of the copy.
                    continue
                if address != attempt["acquire"]:
                    return ("the first load after the fence does not load "
                            "the address of the acquire load (the second "
                            "load)"), False
                attempt["second"] = address
            elif where in ("shared", "unknown"):
                return (f"a load of {where} memory after the second load, "
                        "outside the window"), False
    problem = finish(attempt)
    if problem:
        return problem, False
    return None, fenced_any


def check_repair(events, min_stores):
    """One path of the repair of contract 6.3: every store of shared memory
    is followed by a release store, with at least `min_stores` stores (the
    quality and its flags) before each release store; no fence and no
    acquire load. The odd `seq` and its DMB ISH are those of the write that
    did not finish (repair.litmus)."""
    events = [e for e in events if e[0] != "L"]
    letters = letters_of(events)
    for bad in "AFGXC":
        if bad in letters:
            return f"unexpected instruction ({bad})"
    stores = 0
    for letter, _, _, where in events:
        if letter == "S" and where != "private":
            stores += 1
        elif letter == "R":
            if stores < min_stores:
                return (f"fewer than {min_stores} stores before a release "
                        "store")
            stores = 0
    if stores:
        return "a store after the last release store"
    return None


def check_single(events, letter):
    """One path of a function that is one access of the handshake of a
    RETIRING generation (decision D-54, handshake.litmus): exactly one
    acquire load (`letter` A) or one release store (R), and no other
    barrier, atomic or unknown call. On Armv8.0 a seq_cst load is LDAR and
    a seq_cst store STLR, as acquire and release are."""
    letters = letters_of(events)
    for bad in "FGXC":
        if bad in letters:
            return f"unexpected instruction ({bad})"
    other = "R" if letter == "A" else "A"
    if other in letters:
        return f"unexpected instruction ({other})"
    name = "seq_cst load" if letter == "A" else "seq_cst store"
    if letters.count(letter) != 1:
        return f"{letters.count(letter)} instructions of the {name}, not 1"
    return None


def check_publish(events):
    """One path of publish_write_seq (decision D-54, handshake.litmus): the
    seq_cst store of `write_seq` (STLR), then the seq_cst load of the state
    (LDAR, in HeaderView::retiring, which is followed), and no store after
    the STLR."""
    letters = "".join(e[0] for e in events if e[0] in "ARSFGXC")
    for bad in "FGXC":
        if bad in letters:
            return f"unexpected instruction ({bad})"
    if letters.count("R") != 1:
        return f"{letters.count('R')} seq_cst stores of write_seq, not 1"
    end = letters.index("R")
    if "A" in letters[:end]:
        return "a seq_cst load before the store of write_seq"
    if "A" not in letters[end + 1:]:
        return "no seq_cst load of the state after the store of write_seq"
    if "S" in letters[end + 1:]:
        return "a store after the store of write_seq"
    return None


def check_function(program, function, rule, shared, argument):
    # The repair is a loop over every record with several branches in its
    # body: each branch taken once already gives every order of two
    # iterations, and twice gives too many paths.
    paths = program.paths(function, times=1 if rule == "repair" else 2)
    if not paths:
        return "no path returns"
    any_fenced = False
    any_release = False
    for path in paths:
        events = evaluate(path, shared)
        if rule == "writer":
            problem = check_writer(events, *argument)
        elif rule == "single":
            problem = check_single(events, argument)
        elif rule == "publish":
            problem = check_publish(events)
        elif rule == "repair":
            problem = check_repair(events, argument)
            any_release = any_release or "R" in letters_of(events)
        else:
            problem, fenced = check_reader(events, argument)
            any_fenced = any_fenced or fenced
        if problem:
            return f"{problem}\n    path: {shape(events)}"
    if rule == "reader" and not any_fenced:
        return "no path has an acquire load followed by DMB ISHLD"
    if rule == "repair" and not any_release:
        return "no path has a release store"
    return None


def shape(events):
    return " | ".join(e[1] for e in events)


# Function name prefix (as objdump -C prints it), its side, the argument
# registers that point at shared memory, and the counts: a write stores 6
# or 7 of the 8 fields of TagRecord other than `seq`, a string write also
# `len` (the bytes are none for an empty string), an entry has 7 fields; a
# read copies the 8 fields, a string read also `len` and the bytes (a loop:
# at least 2 loads), an entry read its 7 fields; a repair stores quality
# and flags. Arguments: x0 TagRecord&, x1 StringSlot& (string functions);
# a span takes two registers, data first (the ring; for the repair the
# tags and the strings).
CHECKS = (
    ("scada::tag::write_record(", "writer", {0}, (True, 6)),
    ("scada::tag::write_string_record(", "writer", {0, 1}, (True, 7)),
    ("scada::tag::read_record(", "reader", {0}, 8),
    ("scada::tag::read_string_record(", "reader", {0, 1}, 10),
    ("scada::tag::append_change(", "writer", {0}, (False, 7)),
    ("scada::tag::read_change(", "reader", {0}, 7),
    ("scada::tag::repair_tag_records(", "repair", {0, 2}, 2),
)


# The handshake of a RETIRING generation (decision D-54): each of its
# accesses is in one of these functions (src/queue_region.h). The state load
# of every post and drain is HeaderView::retiring; the RETIRING store of
# scada-tagd is HeaderView::set_retiring; the store of `write_seq` and the
# second state load of a post are publish_write_seq; the `write_seq` load of
# a drain is queue_write_seq_sc. A seq_cst access made relaxed compiles to a
# plain LDR or STR there and fails. Kept apart from CHECKS: the fixture of
# the self-test holds only the functions of the sequence lock and the ring.
HANDSHAKE_CHECKS = (
    ("scada::tag::HeaderView::retiring(", "single", set(), "A"),
    ("scada::tag::HeaderView::set_retiring(", "single", set(), "R"),
    ("scada::tag::queue_write_seq_sc(", "single", set(), "A"),
    ("scada::tag::publish_write_seq(", "publish", set(), None),
)
ALL_CHECKS = CHECKS + HANDSHAKE_CHECKS


def check(disassembly, report=None, checks=CHECKS, findings=None):
    """Returns the list of failures; empty if the code has the shape. With
    `report`, the disassembly of each function that fails, and of the
    functions it calls, is appended to that list. With `findings`, a tuple
    (short function name, problem, whether it is a finding about the
    order) is appended for each failure: not one when the function is
    missing or cannot be walked."""
    program = Program(disassembly)
    failures = []
    for prefix, rule, shared, argument in checks:
        short = prefix.removeprefix("scada::tag::").rstrip("(")
        functions = [f for f in program.find(prefix)
                     if "[clone" not in f.name]
        if len(functions) != 1:
            problem = f"found {len(functions)} definitions"
            failures.append(f"{prefix}...): {problem}")
            if findings is not None:
                findings.append((short, problem, False))
            continue
        order = True
        try:
            problem = check_function(program, functions[0], rule, shared,
                                     argument)
        except StructuralError as error:
            problem, order = str(error), False
        if problem == "no path returns":
            order = False
        if problem:
            failures.append(f"{functions[0].name}: {problem}")
            if findings is not None:
                findings.append((short, problem, order))
            if report is not None:
                report.extend(related(program, functions[0]))
    return failures


def related(program, function, seen=None):
    """The disassembly of `function` and of the functions it calls."""
    seen = set() if seen is None else seen
    if id(function) in seen:
        return []
    seen.add(id(function))
    lines = list(function.lines)
    for instruction in function.code:
        if instruction.symbol and not instruction.symbol.startswith(
                NO_RETURN):
            target = program.resolve(function, instruction.symbol)
            if target is not None and target[0] is not function:
                lines += related(program, target[0], seen)
    return lines


def disassemble(objdump, files):
    """The disassembly of `files`, or raises RuntimeError."""
    result = subprocess.run(
        [objdump, "-d", "-r", "-C", "--no-show-raw-insn", *files],
        capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{objdump} exited with {result.returncode}:\n"
                           f"{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump", required=True, help="path of objdump")
    parser.add_argument("library", nargs="+",
                        help="the tag library (libscada-tag.a) or objects")
    args = parser.parse_args()
    try:
        disassembly = disassemble(args.objdump, args.library)
    except RuntimeError as error:
        print(f"FAIL {error}")
        return 1
    report = []
    failures = check(disassembly, report, checks=ALL_CHECKS)
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        # The code that failed, so that it can be read from the job log.
        print("\nDisassembly of the functions that failed:")
        print("\n".join(report))
        return 1
    print(f"ok   {len(ALL_CHECKS)} functions have the order of the model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
