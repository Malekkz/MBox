#!/usr/bin/env python3
"""Conformance test 4: model check of the sequence lock, the change ring and
the repair after an owner restart.

Runs herd7 with the Armv8 memory model (aarch64.cat) on each litmus test of
this directory, and on copies of each that lack one ordering of the
protocol: for the sequence lock, the ring and the repair either fence, the
release store, the acquire load; for the handshake of a RETIRING generation
each of its six accesses. The test passes when herd7 finds the condition of
every test unreachable ("Never") and the expected result in every weakened
copy: "Sometimes" where the ordering is needed. See README.md.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import time

# (name, what is taken away, pattern, replacement, expected result). The
# patterns match the instructions the protocol compiles to;
# tests/model/code_shape.py checks that the library still compiles to them.
WEAKENINGS = (
    ("no-fence-release", "fence_release (DMB ISH) left out",
     re.compile(r"DMB ISH(?!LD)"), "NOP    ", "Sometimes"),
    ("no-fence-acquire", "fence_acquire (DMB ISHLD) left out",
     re.compile(r"DMB ISHLD"), "NOP      ", "Sometimes"),
    ("relaxed-end-store", "release store of the end of a write made relaxed",
     re.compile(r"STLR "), "STR  ", "Sometimes"),
    ("relaxed-first-load", "acquire load of a read made relaxed",
     re.compile(r"LDAR "), "LDR  ", "Sometimes"),
)

# The handshake of a RETIRING generation (decision D-54). The store of
# write_seq, the second load of a post and the drain's load of the state
# are each needed: without one the loss is reachable. The other three are
# not needed on Armv8 for this outcome (a lone store, a load that an
# acquire load before it already orders, a load before the publication),
# which herd7 shows as Never; C++ needs them seq_cst all the same, and
# code_shape.py keeps them LDAR and STLR.
HANDSHAKE_WEAKENINGS = (
    ("relaxed-publish-store", "store of write_seq of a post made relaxed",
     re.compile(r"STLR W7"), "STR  W7", "Sometimes"),
    ("relaxed-second-load", "second load of the state of a post made relaxed",
     re.compile(r"LDAR W9"), "LDR  W9", "Sometimes"),
    ("relaxed-drain-state-load", "load of the state of a drain made relaxed",
     re.compile(r"LDAR W6"), "LDR  W6", "Sometimes"),
    ("relaxed-retire-store", "RETIRING store made relaxed (Armv8 only)",
     re.compile(r"STLR W5"), "STR  W5", "Never"),
    ("relaxed-drain-write-seq-load",
     "load of write_seq of a drain made relaxed (Armv8 only)",
     re.compile(r"LDAR W8"), "LDR  W8", "Never"),
    ("relaxed-first-load-of-post",
     "first load of the state of a post made relaxed",
     re.compile(r"LDAR W4"), "LDR  W4", "Never"),
)

TESTS = (
    ("seqlock.litmus", WEAKENINGS),
    ("change-ring.litmus", WEAKENINGS),
    ("repair.litmus", WEAKENINGS),
    ("handshake.litmus", HANDSHAKE_WEAKENINGS),
)

OBSERVATION = re.compile(r"^Observation (\S+) (Never|Sometimes|Always)\b",
                         re.MULTILINE)


def run_herd(herd7, libdir, path, timeout):
    command = [herd7, "-model", "aarch64.cat"]
    if libdir:
        command += ["-set-libdir", libdir]
    command.append(str(path))
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=timeout, check=False)
    match = OBSERVATION.search(result.stdout)
    if result.returncode != 0 or match is None:
        raise RuntimeError(
            f"herd7 failed on {path.name} (exit {result.returncode}):\n"
            f"{result.stdout}{result.stderr}")
    return match.group(2), result.stdout


def weaken(text, name, pattern, replacement):
    weakened, count = pattern.subn(replacement, text)
    if count == 0:
        raise RuntimeError(f"weakening {name} matches nothing")
    # A test of its own name, so that herd7's output says which one it is.
    first, rest = weakened.split("\n", 1)
    return f"{first}+{name}\n{rest}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--herd7", required=True, help="path of herd7")
    parser.add_argument("--libdir", default="",
                        help="herd7 library directory, if not its own")
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="seconds for one run of herd7")
    parser.add_argument("--verbose", action="store_true",
                        help="print the whole output of herd7")
    args = parser.parse_args()

    here = pathlib.Path(__file__).resolve().parent
    failures = 0
    with tempfile.TemporaryDirectory() as work:
        for test, weakenings in TESTS:
            source = here / test
            text = source.read_text(encoding="utf-8")
            runs = [(source, "Never", "as written")]
            for name, what, pattern, replacement, expected in weakenings:
                path = pathlib.Path(work) / f"{source.stem}+{name}.litmus"
                path.write_text(weaken(text, name, pattern, replacement),
                                encoding="utf-8")
                runs.append((path, expected, what))
            for path, expected, what in runs:
                start = time.monotonic()
                found, output = run_herd(args.herd7, args.libdir, path,
                                         args.timeout)
                seconds = time.monotonic() - start
                ok = found == expected
                failures += 0 if ok else 1
                print(f"{'ok  ' if ok else 'FAIL'} {source.stem}, {what}: "
                      f"{found} (expected {expected}), {seconds:.1f} s")
                if args.verbose or not ok:
                    print(output)
    if failures:
        print(f"{failures} run(s) did not give the expected result")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
