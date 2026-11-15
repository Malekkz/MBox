#!/usr/bin/env python3
"""Compiles edited copies of the protocol sources and expects code_shape.py
to reject each.

Each edit below is a change of the C++ a reviewer could miss: a fence
dropped or moved, an acquire or release access made relaxed, a load or a
store outside the window. The edited file is compiled with the command the
build used for it (compile_commands.json), so with the compiler and the
flags of this build, and the disassembly of the edited object with the
other objects of the tag library must fail the check of the function the
edit concerns with a finding about the order, not because the function is
missing or cannot be walked. Each source as it is, compiled the same way,
must pass: that shows the recompilation matches the build. See README.md and issue
#50.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import code_shape  # noqa: E402  (after the path is set)

FILES = ("tag_record.cpp", "change_ring.cpp", "attachment.cpp")
WRITERS = ("write_record", "write_string_record")
READERS = ("read_record", "read_string_record")

BEGIN_WRITE = """\
    store_shared(record.seq, seq + 1, RELAXED);
    // On ARM64 a store does not order the stores that follow it.
    fence_release();
"""
UNCHANGED_DURING = """\
    copy();
    // The copies must complete before the second load.
    fence_acquire();
"""
FIRST_LOAD = "load_shared(record.seq, std::memory_order_acquire)"
APPEND = """\
    fence_release();
    store_entry(entry_at(region, written, capacity), entry);
"""
READ_CHANGE_COPY = """\
    const ChangeEntry copy = load_entry(entry_at(region, cursor, capacity));
    // The copy must complete before the second load.
    fence_acquire();
"""
REPAIR_END = \
    "store_shared(record.seq, seq + 1, std::memory_order_release);"

# (name, file, text, replacement, occurrence of the text, functions that
# must fail). The text must occur in the file; the edit applies to the
# occurrence given (0 is the first).
MUTANTS = (
    ("begin_write without its fence", "tag_record.cpp",
     BEGIN_WRITE, BEGIN_WRITE.replace("    fence_release();\n", ""), 0,
     WRITERS),
    ("begin_write with its fence before the odd sequence number",
     "tag_record.cpp", BEGIN_WRITE,
     "    fence_release();\n    store_shared(record.seq, seq + 1, RELAXED);\n",
     0, WRITERS),
    ("end_write relaxed", "tag_record.cpp",
     "store_shared(record.seq, seq + 2, std::memory_order_release);",
     "store_shared(record.seq, seq + 2, RELAXED);", 0, WRITERS),
    ("write_record with a store after end_write", "tag_record.cpp",
     "    end_write(record, seq);\n",
     "    end_write(record, seq);\n"
     "    store_shared(record.change_count, std::uint64_t{0}, RELAXED);\n",
     0, ("write_record",)),
    ("a read without its fence", "tag_record.cpp",
     UNCHANGED_DURING, UNCHANGED_DURING.replace("    fence_acquire();\n", ""),
     0, READERS),
    ("a read with its fence before the copy", "tag_record.cpp",
     UNCHANGED_DURING, "    fence_acquire();\n    copy();\n", 0, READERS),
    ("a read whose second load loads another field", "tag_record.cpp",
     "return load_shared(record.seq, RELAXED) == first_seq;",
     "return load_shared(record.quality_flags, RELAXED) == first_seq;", 0,
     READERS),
    ("read_record with a relaxed first load", "tag_record.cpp",
     FIRST_LOAD, "load_shared(record.seq, RELAXED)", 0, ("read_record",)),
    ("read_string_record with a relaxed first load", "tag_record.cpp",
     FIRST_LOAD, "load_shared(record.seq, RELAXED)", 1,
     ("read_string_record",)),
    ("read_record with a load after the second load", "tag_record.cpp",
     "            out = copy;\n            return true;",
     "            copy.change_count = load_shared(record.change_count, "
     "RELAXED);\n            out = copy;\n            return true;", 0,
     ("read_record",)),
    ("read_string_record with a slot load after the second load",
     "tag_record.cpp", "            text = string;\n",
     "            string.len = load_shared(slot.len, RELAXED);\n"
     "            text = string;\n", 0, ("read_string_record",)),
    ("append_change without its fence", "change_ring.cpp",
     APPEND, APPEND.replace("    fence_release();\n", ""), 0,
     ("append_change",)),
    ("append_change with its fence after the entry", "change_ring.cpp",
     APPEND, "    store_entry(entry_at(region, written, capacity), entry);\n"
     "    fence_release();\n", 0, ("append_change",)),
    ("append_change with a relaxed end store", "change_ring.cpp",
     "store_shared(header.write_seq, written + 1, std::memory_order_release);",
     "store_shared(header.write_seq, written + 1, RELAXED);", 0,
     ("append_change",)),
    ("read_change with a relaxed first load", "change_ring.cpp",
     "        load_shared(header.write_seq, std::memory_order_acquire);\n"
     "    write_seq = first;",
     "        load_shared(header.write_seq, RELAXED);\n"
     "    write_seq = first;", 0, ("read_change",)),
    ("read_change without its fence", "change_ring.cpp",
     READ_CHANGE_COPY, READ_CHANGE_COPY.replace("    fence_acquire();\n", ""),
     0, ("read_change",)),
    ("read_change with its fence before the copy", "change_ring.cpp",
     READ_CHANGE_COPY, "    fence_acquire();\n" + READ_CHANGE_COPY.replace(
         "    fence_acquire();\n", ""), 0, ("read_change",)),
    ("read_change whose second load loads the epoch", "change_ring.cpp",
     "const std::uint64_t second = load_shared(header.write_seq, RELAXED);",
     "const std::uint64_t second = load_shared(header.epoch, RELAXED);", 0,
     ("read_change",)),
    ("read_change with a load of the region after the second load",
     "change_ring.cpp", "    out = copy;\n",
     "    out = copy;\n    out.handle = load_shared("
     "entry_at(region, cursor, capacity).handle, RELAXED);\n", 0,
     ("read_change",)),
    ("the repair with a relaxed end store", "attachment.cpp",
     REPAIR_END, "store_shared(record.seq, seq + 1, "
     "std::memory_order_relaxed);", 0, ("repair_tag_records",)),
    ("the repair with a store after the end store", "attachment.cpp",
     REPAIR_END, REPAIR_END + "\n            store_shared(record.quality, "
     "to_raw(Quality::BAD), std::memory_order_relaxed);", 0,
     ("repair_tag_records",)),
    ("the repair with a fence", "attachment.cpp",
     REPAIR_END, "std::atomic_thread_fence(std::memory_order_seq_cst);\n"
     "            " + REPAIR_END, 0, ("repair_tag_records",)),
)


def replace_occurrence(text, old, new, occurrence):
    start = -1
    for _ in range(occurrence + 1):
        start = text.find(old, start + 1)
        if start < 0:
            return None
    return text[:start] + new + text[start + len(old):]


def compile_commands(path):
    """{file name: (directory, arguments, object path)} of the tag
    library's sources."""
    entries = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    found = {}
    for entry in entries:
        source = pathlib.Path(entry["file"])
        if source.name in FILES and source.parent.name == "src" and \
                source.parent.parent.name == "scada-tag":
            arguments = entry.get("arguments") or shlex.split(entry["command"])
            directory = pathlib.Path(entry["directory"])
            # The object as the command names it, relative to `directory`.
            obj = directory / arguments[arguments.index("-o") + 1]
            found[source.name] = (directory, arguments, source, obj)
    missing = [name for name in FILES if name not in found]
    if missing:
        raise RuntimeError(f"not in {path}: {', '.join(missing)}")
    return found


def build(commands, name, text, work):
    """Compiles `text` as the source `name` and returns the object."""
    directory, arguments, source, _ = commands[name]
    copy = pathlib.Path(work) / name
    copy.write_text(text, encoding="utf-8")
    obj = copy.with_suffix(".o")
    command = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument == "-o":
            skip = True
            continue
        if pathlib.Path(argument) == source or argument == str(source):
            continue
        command.append(argument)
    # The copy is elsewhere: its own directory stays first for its quoted
    # includes. An edit may leave a variable unused; that is not what the
    # test is about.
    command[1:1] = ["-iquote", str(source.parent)]
    command += ["-Wno-error", "-o", str(obj), "-c", str(copy)]
    result = subprocess.run(command, cwd=directory, capture_output=True,
                            text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"compiling {name} failed:\n{shlex.join(command)}"
                           f"\n{result.stderr}")
    return obj


def run(commands, objdump, name, text, expected):
    """Returns a problem, or None if the outcome is as expected."""
    with tempfile.TemporaryDirectory() as work:
        obj = build(commands, name, text, work)
        objects = [str(obj) if file == name else str(commands[file][3])
                   for file in FILES]
        findings = []
        failures = code_shape.check(code_shape.disassemble(objdump, objects),
                                    findings=findings)
    if not expected:
        return None if not failures else "\n".join(failures)
    # Only a finding about the order counts: a function the check could
    # not find or walk is not a rejection of the edit.
    structural = [f"{name}: {problem}" for name, problem, order in findings
                  if not order]
    if structural:
        return f"the check could not judge the edit: {structural}"
    rejected = {name for name, _, order in findings if order}
    missing = [f for f in expected if f not in rejected]
    if missing:
        return (f"not rejected: {', '.join(missing)}; the check reported: "
                f"{failures or 'nothing'}")
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump", required=True, help="path of objdump")
    parser.add_argument("--compile-commands", required=True,
                        help="compile_commands.json of the build")
    args = parser.parse_args()
    try:
        commands = compile_commands(args.compile_commands)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"FAIL {error}")
        return 1
    sources = {name: commands[name][2].read_text(encoding="utf-8")
               for name in FILES}

    # Each source unchanged, compiled the same way, must pass.
    jobs = [(f"{name} as it is", name, sources[name], ()) for name in FILES]
    for name, file, old, new, occurrence, expected in MUTANTS:
        text = replace_occurrence(sources[file], old, new, occurrence)
        if text is None:
            print(f"FAIL {name}: the text to edit is no longer in {file}; "
                  "update tests/model/code_shape_mutants.py")
            return 1
        jobs.append((name, file, text, expected))

    failures = 0
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=os.cpu_count() or 1) as pool:
        futures = [pool.submit(run, commands, args.objdump, file, text,
                               expected)
                   for _, file, text, expected in jobs]
        for (name, _, _, expected), future in zip(jobs, futures):
            try:
                problem = future.result()
            except RuntimeError as error:
                problem = str(error)
            failures += 1 if problem else 0
            outcome = "rejected" if expected else "passes"
            print(f"{'FAIL' if problem else 'ok  '} {name}"
                  f"{': ' + problem if problem else ': ' + outcome}")
    if failures:
        print(f"{failures} of {len(jobs)} did not give the expected result")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
