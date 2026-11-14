#!/usr/bin/env python3
"""Checks that the ARM64 library compiles to the order the litmus tests model.

seqlock.litmus and change-ring.litmus model the instructions the protocol
functions compile to. This script disassembles those functions from the
tag library and checks the order of their memory accesses and barriers, so
that an edit that drops, weakens or moves a fence or an acquire or release
access fails here although the model still passes. See README.md.

Input: the disassembly of `objdump -d -r -C --no-show-raw-insn` (GNU or LLVM)
of an ARM64 object or static library, from a build with optimisation
(at -O0 the atomic accesses are calls, and the check fails).
"""

import argparse
import re
import subprocess
import sys

FUNCTION = re.compile(r"^[0-9a-f]+ <(.+)>:\s*$")
INSTRUCTION = re.compile(r"^\s*[0-9a-f]+:\s+([a-z][a-z0-9.]*)\s*(.*)$")
RELOCATION = re.compile(r"^\s*[0-9a-f]+:\s+R_AARCH64_(?:CALL|JUMP)26\s+(.+)$")

# One letter for each instruction that matters to the order:
#   A  acquire load (LDAR, LDAPR)     R  release store (STLR)
#   F  DMB ISH (fence_release)        G  DMB ISHLD (fence_acquire)
#   L  other load                     S  other store
#   C  call                           X  any other barrier
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
    if mnemonic in ("dsb", "ldaxr", "stlxr") or mnemonic.startswith(
            ("ldadd", "swp", "cas", "ldxr", "stxr", "ldaxp", "stlxp")):
        return "X"
    if mnemonic in ("bl", "blr", "blraa", "blrab"):
        return "C"
    on_stack = "[sp" in operands.replace(" ", "")
    if mnemonic.startswith(("ldr", "ldur", "ldp")) and not on_stack:
        return "L"
    if mnemonic.startswith(("str", "stur", "stp")) and not on_stack:
        return "S"
    return None


def functions(disassembly):
    """Maps each function name to its list of (letter, instruction text)."""
    result = {}
    current = None
    for line in disassembly.splitlines():
        header = FUNCTION.match(line)
        if header:
            current = result.setdefault(header.group(1), [])
            continue
        relocation = RELOCATION.match(line)
        if relocation and current and current[-1][0] == "C":
            # The target of a call in an object file or static library.
            current[-1] = ("C", f"{current[-1][1]} {relocation.group(1)}")
            continue
        instruction = INSTRUCTION.match(line)
        if current is None or not instruction:
            continue
        mnemonic, operands = instruction.group(1), instruction.group(2)
        letter = classify(mnemonic, operands)
        if letter:
            current.append((letter, f"{mnemonic} {operands}".strip()))
    return result


def check_writer(tokens, store_before_fence, min_stores):
    """The write side: sections 6.1 and 8.1. `min_stores`: the fewest
    stores the fields can take; compilers do not merge atomic stores."""
    letters = "".join(t for t, _ in tokens)
    if letters.count("F") != 1:
        return f"expected one DMB ISH, found {letters.count('F')}"
    if "R" not in letters:
        return "no release store (STLR)"
    for bad in "AGX":
        if bad in letters:
            return f"unexpected ordering instruction ({bad})"
    fence = letters.index("F")
    first_release = letters.index("R")
    last_release = letters.rindex("R")
    if first_release < fence:
        return "a release store comes before the fence"
    if letters[fence:first_release].count("S") < min_stores:
        return (f"fewer than {min_stores} stores between the fence and the "
                "release store")
    if "S" in letters[last_release:]:
        return "a store after the release store"
    if "C" in letters[fence:]:
        return "a call after the fence"
    before = letters[:fence]
    if store_before_fence and "S" not in before:
        return "no store before the fence (the odd sequence number)"
    if not store_before_fence and "S" in before:
        return "a store before the fence (an entry store not ordered)"
    return None


def check_reader(tokens, min_loads):
    """The read side: sections 6.2 and 8.2. `min_loads`: the fewest loads
    the copy can take; compilers do not merge atomic loads."""
    letters = "".join(t for t, _ in tokens)
    if letters.count("A") != 1:
        return f"expected one acquire load, found {letters.count('A')}"
    if letters.count("G") != 1:
        return f"expected one DMB ISHLD, found {letters.count('G')}"
    for bad in "FRXC":
        if bad in letters:
            return f"unexpected instruction ({bad})"
    acquire = letters.index("A")
    fence = letters.index("G")
    if fence < acquire:
        return "the fence comes before the acquire load"
    if letters[acquire:fence].count("L") < min_loads:
        return (f"fewer than {min_loads} loads between the acquire load and "
                "the fence")
    if "L" not in letters[fence:]:
        return "no load after the fence (the second load)"
    return None


# Function name prefix (as objdump -C prints it), and its check. The counts
# are the fields of TagRecord other than `seq` (8; a write stores 6 or 7),
# a string slot (`len`, 250 bytes in a loop: at least 2 instructions) and
# ChangeEntry (7).
CHECKS = (
    ("scada::tag::write_record(", lambda t: check_writer(t, True, 6)),
    ("scada::tag::write_string_record(",
     lambda t: check_writer(t, True, 8)),
    ("scada::tag::read_record(", lambda t: check_reader(t, 8)),
    ("scada::tag::read_string_record(", lambda t: check_reader(t, 10)),
    ("scada::tag::append_change(", lambda t: check_writer(t, False, 7)),
    ("scada::tag::read_change(", lambda t: check_reader(t, 7)),
)


def check(disassembly):
    """Returns the list of failures; empty if the code has the shape."""
    found = functions(disassembly)
    failures = []
    for prefix, rule in CHECKS:
        names = [n for n in found if n.startswith(prefix)]
        if len(names) != 1:
            failures.append(f"{prefix}...): found {len(names)} definitions")
            continue
        # `panic` does not return, so no access after it is reordered with
        # anything: its calls (bounds checks) are left out.
        tokens = [t for t in found[names[0]]
                  if not (t[0] == "C" and "scada::panic(" in t[1])]
        problem = rule(tokens)
        if problem:
            shape = " ".join(text for _, text in tokens)
            failures.append(f"{names[0]}: {problem}\n    {shape}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump", required=True, help="path of objdump")
    parser.add_argument("library", help="the tag library (libscada-tag.a)")
    args = parser.parse_args()
    disassembly = subprocess.run(
        [args.objdump, "-d", "-r", "-C", "--no-show-raw-insn", args.library],
        capture_output=True, text=True, check=True).stdout
    failures = check(disassembly)
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        return 1
    print(f"ok   {len(CHECKS)} functions have the order of the model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
