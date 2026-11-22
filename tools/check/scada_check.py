#!/usr/bin/env python3
"""Static checks of the `check` build target.

Runs four checks over the C++ sources and fails on any finding
(docs/coding-standard.md, section 13):

  format  clang-format, configuration in .clang-format
  tidy    clang-tidy, configuration in .clang-tidy; every finding is an error
  banned  the banned list of the coding standard, section 12 (scada-banned)
  rt      calls that a SCADA_RT function must not make (scada-rt)

`banned` and `rt` are sets of clang-query matchers, defined below.

`tidy` lints every translation unit unless --tidy-base is given; then it
lints only the units the changes since that revision could have affected
(lint_selection.py). The other checks always run on every file.

Exit status: 0 no findings, 1 findings, 2 the checks could not run.
"""

import argparse
import concurrent.futures
import dataclasses
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import traceback

import lint_selection

MINIMUM_LLVM_MAJOR = 18
SOURCE_ROOTS = ("libs", "services", "tools", "tests")
HEADER_SUFFIXES = (".h", ".hpp")
UNIT_SUFFIXES = (".cpp", ".cc", ".cxx")
ALL_CHECKS = ("format", "tidy", "banned", "rt")

EXIT_CLEAN = 0
EXIT_FINDINGS = 1
EXIT_CANNOT_RUN = 2


class CheckError(Exception):
    """The checks could not run: a tool is missing or failed, or bad input."""


@dataclasses.dataclass(frozen=True)
class Rule:
    check: str  # Name used in the report and in NOLINT(...).
    matcher: str  # clang-query matcher.
    message: str


def _names(*names):
    """Quoted names for hasAnyName, in the global and the std namespace."""
    qualified = []
    for name in names:
        qualified += [f'"::{name}"', f'"::std::{name}"']
    return ", ".join(qualified)


def _uses(where, *functions):
    """Matches any use of the functions: a call, or taking the address."""
    return (f"declRefExpr({where}, "
            f"to(functionDecl(hasAnyName({_names(*functions)}))))")


def _mutex_calls(where, *methods):
    """Matches calls of the named member functions on a standard mutex."""
    quoted = ", ".join(f'"{method}"' for method in methods)
    mutexes = _names("mutex", "timed_mutex", "recursive_mutex",
                     "recursive_timed_mutex", "shared_mutex",
                     "shared_timed_mutex")
    return (f"cxxMemberCallExpr({where}, callee(cxxMethodDecl("
            f"hasAnyName({quoted}), ofClass(hasAnyName({mutexes})))))")


# Every matcher starts with one of these two conditions.
#
# Matches in system headers are of no interest and are many.
_ANY = "unless(isExpansionInSystemHeader())"
# A function is real-time if it carries SCADA_RT, which expands to an
# `annotate` attribute. Lambdas inside such a function count as part of it.
_RT = _ANY + ', hasAncestor(functionDecl(hasAttr("attr::Annotate")))'

_ALLOCATION = ("malloc", "calloc", "realloc", "free", "aligned_alloc")

# GoogleTest registers each test with a `new` written in the body of this
# macro, which TEST and TEST_F expand to. The expression is the framework's
# code, not ours, but it is expanded in our files, so _ANY does not cover it.
# A `new` in an argument of the macro, or in the test body, is still reported.
_FRAMEWORK_NEW = 'unless(isExpandedFromMacro("GTEST_TEST_"))'

# Coding standard, section 12. One rule per table row where possible; the
# message names the replacement the table gives.
BANNED_RULES = [
    Rule("scada-banned", f"cxxNewExpr({_ANY}, {_FRAMEWORK_NEW})",
         "'new' is banned; use a value, std::make_unique or a container"),
    Rule("scada-banned", f"cxxDeleteExpr({_ANY})",
         "'delete' is banned; the owner's destructor releases the resource"),
    Rule("scada-banned", _uses(_ANY, *_ALLOCATION),
         "the C allocation functions are banned; use a value, "
         "std::make_unique or a container"),
    Rule("scada-banned",
         _uses(_ANY, "strcpy", "strcat", "sprintf", "vsprintf", "gets",
               "strtok"),
         "unbounded C string functions are banned; use std::string, "
         "std::format_to_n or a checked helper"),
    Rule("scada-banned", _uses(_ANY, "strncpy", "strlen"),
         "strncpy and strlen are banned; use a length-checked helper on a "
         "span"),
    Rule("scada-banned",
         _uses(_ANY, "atoi", "atol", "atoll", "atof", "strtol", "strtoll",
               "strtoul", "strtoull", "strtof", "strtod", "strtold"),
         "C number parsing is banned; use std::from_chars"),
    Rule("scada-banned",
         _uses(_ANY, "scanf", "fscanf", "sscanf", "vscanf", "vfscanf",
               "vsscanf"),
         "the scanf family is banned; use a parser with a checked cursor"),
    Rule("scada-banned", _uses(_ANY, "alloca", "__builtin_alloca"),
         "alloca is banned; use std::array or a pre-sized container"),
    Rule("scada-banned", f"varDecl({_ANY}, hasType(variableArrayType()))",
         "variable-length arrays are banned; use std::array or a pre-sized "
         "container"),
    Rule("scada-banned",
         f"cStyleCastExpr({_ANY}, unless(hasDestinationType(voidType())))",
         "C-style casts are banned; use a named cast or a checked helper"),
    Rule("scada-banned", f"cxxConstCastExpr({_ANY})", "const_cast is banned"),
    Rule("scada-banned", f"cxxReinterpretCastExpr({_ANY})",
         "reinterpret_cast is banned outside the reviewed helpers; use "
         "std::bit_cast or a span-based helper"),
    Rule("scada-banned", _uses(_ANY, "memcpy", "memmove", "memset"),
         "raw memcpy, memmove and memset are banned outside the reviewed "
         "helpers; use std::copy, std::fill or std::bit_cast"),
    Rule("scada-banned", _uses(_ANY, "rand", "srand"),
         "rand is banned; use <random>, or the operating system's source "
         "for security use"),
    Rule("scada-banned", _uses(_ANY, "system", "popen"),
         "system and popen are banned; use direct system calls with fixed "
         "arguments, in the privileged helper only"),
    Rule("scada-banned",
         f"usingDirectiveDecl({_ANY}, "
         'isExpansionInFileMatching("[.](h|hpp)$"))',
         "'using namespace' in a header is banned; use qualified names"),
    Rule("scada-banned",
         f'cxxMemberCallExpr({_ANY}, callee(cxxMethodDecl(hasName("detach"), '
         f"ofClass(hasAnyName({_names('thread', 'jthread')})))))",
         "detached threads are banned; use std::jthread and let it join"),
    Rule("scada-banned",
         _mutex_calls(_ANY, "lock", "try_lock", "unlock", "lock_shared",
                      "try_lock_shared", "unlock_shared"),
         "manual lock and unlock are banned; use std::scoped_lock or "
         "std::unique_lock"),
    Rule("scada-banned", f"gotoStmt({_ANY})",
         "goto is banned; use structured control flow"),
    Rule("scada-banned",
         _uses(_ANY, "setjmp", "_setjmp", "__sigsetjmp", "sigsetjmp",
               "longjmp", "siglongjmp"),
         "setjmp and longjmp are banned; use structured control flow"),
]

# First version of the real-time check: direct calls to a small deny-list.
# It does not follow calls into other functions.
RT_RULES = [
    Rule("scada-rt", f"cxxNewExpr({_RT})",
         "allocation with 'new' in a SCADA_RT function"),
    Rule("scada-rt", f"cxxDeleteExpr({_RT})",
         "'delete' in a SCADA_RT function"),
    Rule("scada-rt",
         _uses(_RT, *_ALLOCATION, "make_unique", "make_shared",
               "allocate_shared"),
         "memory allocation or release in a SCADA_RT function"),
    Rule("scada-rt",
         _uses(_RT, "printf", "fprintf", "vprintf", "vfprintf", "puts",
               "fputs", "putchar", "fputc", "fwrite", "perror"),
         "output to a stream in a SCADA_RT function"),
    Rule("scada-rt",
         _mutex_calls(_RT, "lock", "try_lock", "lock_shared",
                      "try_lock_shared"),
         "mutex lock in a SCADA_RT function"),
    Rule("scada-rt",
         f"cxxConstructExpr({_RT}, hasDeclaration(cxxConstructorDecl(ofClass("
         "hasAnyName("
         + _names("lock_guard", "scoped_lock", "unique_lock", "shared_lock")
         + ")))))",
         "mutex lock in a SCADA_RT function"),
    Rule("scada-rt",
         _uses(_RT, "pthread_mutex_lock", "pthread_mutex_timedlock",
               "pthread_mutex_trylock"),
         "mutex lock in a SCADA_RT function"),
]


def tool_major_version(path):
    """Returns the LLVM major version of the tool at `path`."""
    try:
        result = subprocess.run([path, "--version"], capture_output=True,
                                text=True, check=False)
    except OSError as error:
        raise CheckError(f"cannot run '{path}': {error}") from error
    found = re.search(r"version (\d+)\.", result.stdout)
    if result.returncode != 0 or not found:
        raise CheckError(f"'{path} --version' gave no version")
    return int(found.group(1))


def require_tool(name, path):
    if not path:
        raise CheckError(f"{name} was not found; pass --{name}")
    major = tool_major_version(path)
    if major < MINIMUM_LLVM_MAJOR:
        raise CheckError(f"{name} {major} is too old; version "
                         f"{MINIMUM_LLVM_MAJOR} or newer is needed")
    return path


def is_inside(path, directory):
    return directory == path or directory in path.parents


def discover_sources(source_dir, excluded):
    """All C++ files under the source roots, sorted, minus excluded trees."""
    found = []
    for root in SOURCE_ROOTS:
        for directory, subdirectories, files in os.walk(source_dir / root):
            here = pathlib.Path(directory)
            subdirectories[:] = sorted(
                name for name in subdirectories
                if not any(is_inside(here / name, tree) for tree in excluded))
            for name in sorted(files):
                if name.endswith(HEADER_SUFFIXES + UNIT_SUFFIXES):
                    found.append(here / name)
    return found


def units_in_database(build_dir):
    """Resolved paths of the translation units in compile_commands.json."""
    database = build_dir / "compile_commands.json"
    try:
        with open(database, encoding="utf-8") as stream:
            entries = json.load(stream)
        return {
            (pathlib.Path(entry["directory"]) / entry["file"]).resolve()
            for entry in entries
        }
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise CheckError(f"cannot read '{database}': {error}; configure the "
                         "build first") from error


def run_tool(command):
    try:
        return subprocess.run(command, capture_output=True, text=True,
                              check=False)
    except OSError as error:
        raise CheckError(f"cannot run '{command[0]}': {error}") from error


class Checker:
    def __init__(self, options):
        self.options = options
        self.source_dir = options.source_dir.resolve()
        self.build_dir = (options.build_dir.resolve()
                          if options.build_dir else None)
        # Without explicit flags the compile commands of the build are used.
        self.compiler_arguments = (
            ["--"] + options.compile_flag if options.compile_flag
            else ["-p", str(self.build_dir)])

    def in_parallel(self, function, items):
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=self.options.jobs) as pool:
            return list(pool.map(function, items))

    def check_format(self, files):
        """Returns one report per file whose layout differs."""
        tool = require_tool("clang-format", self.options.clang_format)
        style = f"--style=file:{self.source_dir / '.clang-format'}"

        def one(path):
            result = run_tool(
                [tool, style, "--dry-run", "--Werror", str(path)])
            if result.returncode == 0:
                return None
            return result.stderr.rstrip() or f"{path}: clang-format failed"

        return [report for report in self.in_parallel(one, files) if report]

    def check_tidy(self, units):
        """Returns one report per translation unit with findings."""
        tool = require_tool("clang-tidy", self.options.clang_tidy)
        config = f"--config-file={self.source_dir / '.clang-tidy'}"

        def one(path):
            # .clang-tidy makes only some groups errors, for editors. Here
            # every finding fails (coding standard, section 13).
            result = run_tool(
                [tool, config, "--quiet", "--warnings-as-errors=*", str(path)]
                + self.compiler_arguments)
            if result.returncode == 0:
                return None
            return (result.stdout.rstrip()
                    or result.stderr.rstrip()
                    or f"{path}: clang-tidy failed")

        return [report for report in self.in_parallel(one, units) if report]

    def check_matchers(self, units, rules):
        """Runs clang-query rules; returns sorted, de-duplicated reports."""
        tool = require_tool("clang-query", self.options.clang_query)
        commands = ["set bind-root true", "set output diag"]
        commands += ["match " + rule.matcher for rule in rules]
        with tempfile.NamedTemporaryFile(
                "w", suffix=".query", encoding="utf-8") as script:
            script.write("\n".join(commands) + "\n")
            script.flush()

            def one(path):
                result = run_tool([tool, "-f", script.name, str(path)]
                                  + self.compiler_arguments)
                return path, result

            results = self.in_parallel(one, units)

        findings = set()
        for path, result in results:
            findings |= self.parse_matches(path, result, rules)
        return [
            f"{file}:{line}:{column}: error: {message} [{check}]\n"
            f"    {self.source_line(file, line).strip()}"
            for file, line, column, check, message in sorted(findings)
        ]

    def parse_matches(self, path, result, rules):
        """Turns clang-query output into (file, line, column, check, message).

        Each `match` command prints its matches and then a count line, in
        the order of `rules`.
        """
        location = re.compile(r'^(.+):(\d+):(\d+): note: "root" binds here$')
        count = re.compile(r"^\d+ match(es)?\.$")
        findings = set()
        rule_index = 0
        for text in result.stdout.splitlines():
            if count.match(text):
                rule_index += 1
                continue
            found = location.match(text)
            if not found or rule_index >= len(rules):
                continue
            file = pathlib.Path(found.group(1)).resolve()
            line = int(found.group(2))
            rule = rules[rule_index]
            if (self.is_project_file(file)
                    and not self.is_suppressed(file, line, rule.check)):
                findings.add((str(file), line, int(found.group(3)),
                              rule.check, rule.message))
        # clang-query exits with 0 even if the file did not compile, and
        # then matches on an incomplete syntax tree.
        compile_error = re.search(r": (fatal )?error: ", result.stderr)
        if (result.returncode != 0 or compile_error
                or rule_index != len(rules)):
            raise CheckError(
                f"clang-query failed on '{path}':\n"
                f"{result.stderr.rstrip() or result.stdout.rstrip()}")
        return findings

    def is_project_file(self, file):
        if self.build_dir and is_inside(file, self.build_dir):
            return False
        return is_inside(file, self.source_dir)

    @staticmethod
    def source_line(file, line):
        try:
            with open(file, encoding="utf-8", errors="replace") as stream:
                lines = stream.read().splitlines()
        except OSError:
            return ""
        return lines[line - 1] if 0 < line <= len(lines) else ""

    def is_suppressed(self, file, line, check):
        """True for `// NOLINT(<check>): reason` on the line, or
        NOLINTNEXTLINE on the line before (coding standard, section 14).
        A NOLINT that names no check does not suppress these checks."""
        def names(text, marker):
            found = re.search(marker + r"\(([^)]*)\)", text)
            return [name.strip() for name in found.group(1).split(",")] \
                if found else []

        return (check in names(self.source_line(file, line), r"NOLINT")
                or check in names(self.source_line(file, line - 1),
                                  r"NOLINTNEXTLINE"))


def select_files(options, checker):
    """Returns (files to format, translation units to analyse)."""
    if options.files:
        files = [path.resolve() for path in options.files]
        missing = [str(path) for path in files if not path.is_file()]
        if missing:
            raise CheckError("no such file: " + ", ".join(missing))
    else:
        excluded = [(checker.source_dir / tree).resolve()
                    for tree in options.exclude]
        files = discover_sources(checker.source_dir, excluded)
    units = [path for path in files if path.name.endswith(UNIT_SUFFIXES)]

    needs_units = set(options.only) & {"tidy", "banned", "rt"}
    if needs_units and not options.compile_flag:
        if not checker.build_dir:
            raise CheckError("pass --build-dir or --compile-flag")
        known = units_in_database(checker.build_dir)
        unknown = [str(path) for path in units if path not in known]
        if unknown:
            # Never skip a file silently: it would go unchecked.
            raise CheckError(
                "not part of the build, so they cannot be analysed: "
                + ", ".join(unknown))
    return files, units


def tidy_selection(options, checker, units):
    """Returns the lint_selection.Selection of units for clang-tidy.

    Any failure to work out the changed set lints everything: an error must
    never make the set smaller."""
    if not options.tidy_base:
        return lint_selection.everything("no base revision given")
    if options.compile_flag or options.files:
        return lint_selection.everything(
            "files or compile flags given on the command line")
    try:
        merge_base, changes = lint_selection.changes_since(
            checker.source_dir, options.tidy_base)
        # The rules are looked at first: a full lint needs no dependencies.
        selection = lint_selection.select_units(changes, {},
                                                checker.source_dir)
        if selection.everything:
            return selection
        if changes:
            database = checker.build_dir / "compile_commands.json"
            with open(database, encoding="utf-8") as stream:
                entries = json.load(stream)
            depends = lint_selection.dependencies(entries, units,
                                                  checker.options.jobs)
            selection = lint_selection.select_units(changes, depends,
                                                    checker.source_dir)
        return dataclasses.replace(
            selection, reason=f"{len(changes)} files changed since the "
            f"merge base {merge_base[:12]} of {options.tidy_base} and HEAD")
    except lint_selection.SelectionError as error:
        return lint_selection.everything(
            f"the changed files cannot be worked out: {error}")
    except Exception as error:
        # A bug here must not leave files unlinted.
        traceback.print_exc()
        return lint_selection.everything(
            f"the selection failed: {type(error).__name__}: {error}")


def parse_arguments(arguments):
    parser = argparse.ArgumentParser(
        description="Formatting, lint, banned-function and SCADA_RT checks. "
        "Exit status: 0 no findings, 1 findings, 2 could not run.")
    parser.add_argument("--source-dir", type=pathlib.Path, required=True,
                        help="repository root, with .clang-format and "
                        ".clang-tidy")
    parser.add_argument("--build-dir", type=pathlib.Path,
                        help="build directory with compile_commands.json")
    parser.add_argument("--clang-format", help="path of clang-format")
    parser.add_argument("--clang-tidy", help="path of clang-tidy")
    parser.add_argument("--clang-query", help="path of clang-query")
    parser.add_argument("--only", action="append", choices=ALL_CHECKS,
                        help="run only this check; may be repeated "
                        "(default: all)")
    parser.add_argument("--exclude", action="append", default=[],
                        metavar="DIR", help="directory, relative to the "
                        "source directory, left out when files are searched "
                        "for; may be repeated")
    parser.add_argument("--compile-flag", action="append", default=[],
                        metavar="FLAG", help="compile the files with these "
                        "flags instead of the build's commands, for example "
                        "--compile-flag=-std=c++20; may be repeated")
    parser.add_argument("--tidy-base", metavar="REVISION",
                        help="lint with clang-tidy only the translation "
                        "units that the changes since the merge base of "
                        "REVISION and HEAD could have affected; everything "
                        "if that cannot be worked out (default: lint every "
                        "unit)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="tools run in parallel (default: all "
                        "processors)")
    parser.add_argument("files", nargs="*", type=pathlib.Path,
                        help="files to check (default: every C++ file under "
                        + ", ".join(SOURCE_ROOTS) + ")")
    options = parser.parse_args(arguments)
    options.only = options.only or list(ALL_CHECKS)
    options.jobs = max(1, options.jobs)
    return options


def main(arguments):
    options = parse_arguments(arguments)
    try:
        checker = Checker(options)
        files, units = select_files(options, checker)
        reports = []
        if "format" in options.only:
            reports += checker.check_format(files)
        if "tidy" in options.only:
            selection = tidy_selection(options, checker, units)
            tidy_units = units if selection.everything else list(
                selection.units)
            mode = "everything" if selection.everything else "changed files"
            print(f"check: tidy: {mode} ({selection.reason}): "
                  f"{len(tidy_units)} of {len(units)} translation units")
            if not selection.everything:
                for unit in tidy_units:
                    print(f"check: tidy: {unit}")
            sys.stdout.flush()
            reports += checker.check_tidy(tidy_units)
        # One clang-query run per unit for both rule sets: the parse of the
        # unit is most of the time.
        rules = []
        if "banned" in options.only:
            rules += BANNED_RULES
        if "rt" in options.only:
            rules += RT_RULES
        if rules:
            reports += checker.check_matchers(units, rules)
    except CheckError as error:
        print(f"check: {error}", file=sys.stderr)
        return EXIT_CANNOT_RUN

    for report in reports:
        print(report)
    summary = (f"check: {len(files)} files, checks: "
               f"{', '.join(options.only)}")
    if reports:
        print(f"{summary}: FAILED", file=sys.stderr)
        return EXIT_FINDINGS
    print(f"{summary}: no findings")
    return EXIT_CLEAN


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
