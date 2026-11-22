#!/usr/bin/env python3
"""Tests of lint_selection.py and of `--tidy-base` in scada_check.py.

They build a small project in a temporary directory, take its dependencies
from the real compiler, and run the selection on lists of changes. The
tests of `--tidy-base` also make a git repository and run clang-tidy.

Usage: selection_test.py --compiler PATH --clang-format PATH
                         --clang-tidy PATH --clang-query PATH
"""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR.parent))

import lint_selection  # noqa: E402
from lint_selection import Change  # noqa: E402

SCRIPT = TESTS_DIR.parent / "scada_check.py"
REPOSITORY = TESTS_DIR.parents[2]

EXIT_CLEAN = 0
EXIT_FINDINGS = 1

# Set from the command line before the tests run.
COMPILER = []
TOOLS = []

# The project: inner.h is included by one.cpp only through outer.h, and
# shared.h by two.cpp and three.cpp. four.cpp includes no project header.
FILES = {
    "libs/demo/include/demo/inner.h":
        "#pragma once\n\nnamespace demo {\ninline int inner() { return 1; }\n"
        "}  // namespace demo\n",
    "libs/demo/include/demo/outer.h":
        '#pragma once\n\n#include "demo/inner.h"\n\nnamespace demo {\n'
        "inline int outer() { return inner() + 1; }\n}  // namespace demo\n",
    "libs/demo/include/demo/shared.h":
        "#pragma once\n\nnamespace demo {\ninline int shared() { return 2; }\n"
        "}  // namespace demo\n",
    "libs/demo/src/one.cpp":
        '#include "demo/outer.h"\n\nint one() { return demo::outer(); }\n',
    "libs/demo/src/two.cpp":
        '#include "demo/shared.h"\n\nint two() { return demo::shared(); }\n',
    "libs/demo/src/three.cpp":
        '#include "demo/shared.h"\n\n'
        "int three() { return demo::shared() + 1; }\n",
    "libs/demo/src/four.cpp": "int four() { return 4; }\n",
    "docs/notes.md": "# Notes\n",
}
UNITS = ("one", "two", "three", "four")


def make_project(root):
    """Writes the project and its compile_commands.json under `root`."""
    for name, text in FILES.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    shutil.copy(REPOSITORY / ".clang-tidy", root / ".clang-tidy")
    shutil.copy(REPOSITORY / ".clang-format", root / ".clang-format")
    build = root / "build"
    build.mkdir()
    entries = [{
        "directory": str(build),
        "command": " ".join(COMPILER + [
            "-std=c++20", f"-I{root / 'libs/demo/include'}", "-MD", "-MT",
            f"{unit}.o", "-MF", f"{unit}.o.d", "-o", f"{unit}.o", "-c",
            str(root / f"libs/demo/src/{unit}.cpp")]),
        "file": str(root / f"libs/demo/src/{unit}.cpp"),
    } for unit in UNITS]
    (build / "compile_commands.json").write_text(json.dumps(entries),
                                                 encoding="utf-8")
    (root / ".gitignore").write_text("build/\n", encoding="utf-8")
    return entries


class Selection(unittest.TestCase):
    """select_units on the dependencies the compiler reports."""

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = pathlib.Path(cls.temporary.name).resolve()
        entries = make_project(cls.root)
        cls.units = [cls.unit(name) for name in UNITS]
        cls.dependencies = lint_selection.dependencies(entries, cls.units, 4)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @classmethod
    def unit(cls, name):
        return cls.root / f"libs/demo/src/{name}.cpp"

    def select(self, *changes):
        return lint_selection.select_units(changes, self.dependencies,
                                           self.root)

    def assert_units(self, selection, *names):
        self.assertFalse(selection.everything, selection.reason)
        self.assertEqual(list(selection.units),
                         sorted(self.unit(name) for name in names))

    def test_dependencies_come_from_the_compiler(self):
        include = self.root / "libs/demo/include/demo"
        self.assertIn(include / "inner.h", self.dependencies[self.unit("one")])
        self.assertIn(include / "outer.h", self.dependencies[self.unit("one")])
        self.assertNotIn(include / "shared.h",
                         self.dependencies[self.unit("one")])
        self.assertIn(self.unit("four"), self.dependencies[self.unit("four")])

    def test_changed_source_file(self):
        self.assert_units(self.select(Change("M", "libs/demo/src/four.cpp")),
                          "four")

    def test_changed_header_with_two_dependents(self):
        self.assert_units(
            self.select(Change("M", "libs/demo/include/demo/shared.h")),
            "two", "three")

    def test_header_included_through_another_header(self):
        self.assert_units(
            self.select(Change("M", "libs/demo/include/demo/inner.h")), "one")

    def test_changed_clang_tidy_lints_everything(self):
        selection = self.select(Change("M", "docs/notes.md"),
                                Change("M", ".clang-tidy"))
        self.assertTrue(selection.everything)
        self.assertIn(".clang-tidy", selection.reason)

    def test_deleted_header_lints_everything(self):
        selection = self.select(Change("D", "libs/demo/include/demo/inner.h"),
                                Change("M", "libs/demo/include/demo/outer.h"))
        self.assertTrue(selection.everything)
        self.assertIn("inner.h", selection.reason)

    def test_renamed_header_lints_everything(self):
        selection = self.select(Change("R", "libs/demo/include/demo/new.h",
                                       "libs/demo/include/demo/shared.h"))
        self.assertTrue(selection.everything)

    def test_documents_only_lint_nothing(self):
        self.assert_units(self.select(Change("M", "docs/notes.md"),
                                      Change("A", "README.md")))

    def test_deleted_and_renamed_units(self):
        """A deleted unit leaves nothing to lint; a renamed one is linted at
        its new path."""
        self.assert_units(self.select(Change("D", "libs/demo/src/gone.cpp"),
                                      Change("R", "libs/demo/src/four.cpp",
                                             "libs/demo/src/old.cpp")),
                          "four")

    def test_rule_and_build_files_lint_everything(self):
        for path in ("CMakeLists.txt", "libs/demo/CMakeLists.txt",
                     "cmake/ScadaCompileOptions.cmake",
                     "cmake/toolchains/aarch64-linux-gnu.cmake",
                     "CMakePresets.json", ".github/workflows/pipeline.yml",
                     ".github/actions/setup-build-machine/action.yml",
                     "tools/check/scada_check.py",
                     "tools/check/lint_selection.py",
                     "third-party/dependencies.json",
                     "libs/demo/include/demo/version.h.in"):
            with self.subTest(path=path):
                self.assertTrue(self.select(Change("M", path)).everything)
                # A rename away from a rule file counts as well.
                self.assertTrue(self.select(
                    Change("R", "docs/moved.txt", path)).everything)

    def test_failing_compiler_is_an_error(self):
        root = self.root
        entry = {"directory": str(root),
                 "command": " ".join(COMPILER + [
                     "-std=c++20", "-c", str(root / "missing.cpp")]),
                 "file": str(root / "missing.cpp")}
        with self.assertRaises(lint_selection.SelectionError):
            lint_selection.dependencies([entry], [root / "missing.cpp"], 1)

    def test_unit_without_command_is_an_error(self):
        with self.assertRaises(lint_selection.SelectionError):
            lint_selection.dependencies([], [self.unit("one")], 1)


class Parsing(unittest.TestCase):
    def test_name_status(self):
        text = ("M\0a.cpp\0R087\0old.h\0new.h\0D\0gone.h\0A\0b c.md\0"
                "T\0link\0")
        self.assertEqual(lint_selection.parse_name_status(text), [
            Change("M", "a.cpp"), Change("R", "new.h", "old.h"),
            Change("D", "gone.h"), Change("A", "b c.md"),
            Change("T", "link")])

    def test_name_status_unknown_or_cut_short(self):
        for text in ("U\0a.cpp\0", "R100\0old.h\0", "M\0"):
            with self.subTest(text=text):
                with self.assertRaises(lint_selection.SelectionError):
                    lint_selection.parse_name_status(text)

    def test_make_rule(self):
        directory = pathlib.Path("/build")
        text = ("x.o: /src/x.cpp include/a\\ b.h \\\n /src/c$$.h \\\n"
                " /src/d\\#.h\n")
        self.assertEqual(
            lint_selection.parse_make_dependencies(text, directory),
            {pathlib.Path("/src/x.cpp"), pathlib.Path("/build/include/a b.h"),
             pathlib.Path("/src/c$.h"), pathlib.Path("/src/d#.h")})

    def test_dependency_command_drops_the_output(self):
        entry = {"arguments": ["c++", "-Iinc", "-MD", "-MT", "x.o", "-MF",
                               "x.o.d", "-o", "x.o", "-c", "x.cpp"]}
        self.assertEqual(lint_selection.dependency_command(entry),
                         ["c++", "-Iinc", "x.cpp", "-M"])


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), *arguments], check=True,
                   capture_output=True, text=True)


def commit_all(root, message):
    git(root, "add", "--all")
    git(root, "-c", "user.name=test", "-c", "user.email=test@invalid",
        "commit", "--quiet", "--no-verify", "-m", message)


class TidyBase(unittest.TestCase):
    """scada_check.py --tidy-base on a git repository of the project."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name).resolve() / "repo"
        self.root.mkdir()
        make_project(self.root)
        git(self.root, "init", "--quiet", "--initial-branch=main")
        commit_all(self.root, "base")
        git(self.root, "checkout", "--quiet", "-b", "change")

    def tearDown(self):
        self.temporary.cleanup()

    def run_check(self, *arguments, root=None):
        root = root or self.root
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--source-dir", str(root),
             "--build-dir", str(root / "build"), "--only", "tidy"]
            + TOOLS + list(arguments),
            capture_output=True, text=True, check=False)

    def write(self, name, text):
        (self.root / name).write_text(text, encoding="utf-8")

    def test_documents_only_lint_nothing_and_pass(self):
        self.write("docs/notes.md", "# Notes\n\nMore.\n")
        commit_all(self.root, "documents")
        result = self.run_check("--tidy-base", "main")
        self.assertEqual(result.returncode, EXIT_CLEAN,
                         result.stdout + result.stderr)
        self.assertIn("check: tidy: changed files (files changed since",
                      result.stdout)
        self.assertIn("0 of 4 translation units", result.stdout)

    def test_fault_in_header_fails_an_unchanged_dependent(self):
        """The planted fault: a finding that a changed header causes in a
        unit that did not change."""
        self.write("libs/demo/include/demo/inner.h",
                   FILES["libs/demo/include/demo/inner.h"]
                   + "\n#define DEMO_SQUARE(x) ((x) * (x))\n")
        commit_all(self.root, "fault")
        result = self.run_check("--tidy-base", "main")
        self.assertEqual(result.returncode, EXIT_FINDINGS,
                         result.stdout + result.stderr)
        self.assertIn("1 of 4 translation units", result.stdout)
        self.assertIn("libs/demo/src/one.cpp", result.stdout)
        self.assertIn("cppcoreguidelines-macro-usage", result.stdout)

    def test_uncommitted_changes_count(self):
        self.write("libs/demo/src/two.cpp",
                   FILES["libs/demo/src/two.cpp"] + "// Changed.\n")
        self.write("libs/demo/include/demo/inner.h",
                   FILES["libs/demo/include/demo/inner.h"] + "// Changed.\n")
        result = self.run_check("--tidy-base", "main")
        self.assertEqual(result.returncode, EXIT_CLEAN,
                         result.stdout + result.stderr)
        self.assertIn("2 of 4 translation units", result.stdout)

    def test_without_base_everything_is_linted(self):
        result = self.run_check()
        self.assertEqual(result.returncode, EXIT_CLEAN,
                         result.stdout + result.stderr)
        self.assertIn("check: tidy: everything (no base revision given): "
                      "4 of 4 translation units", result.stdout)

    def test_unknown_base_lints_everything(self):
        result = self.run_check("--tidy-base", "no-such-branch")
        self.assertEqual(result.returncode, EXIT_CLEAN,
                         result.stdout + result.stderr)
        self.assertIn("check: tidy: everything (the changed files cannot be "
                      "worked out", result.stdout)
        self.assertIn("4 of 4 translation units", result.stdout)

    def test_no_merge_base_lints_everything(self):
        git(self.root, "checkout", "--quiet", "--orphan", "unrelated")
        commit_all(self.root, "unrelated")
        result = self.run_check("--tidy-base", "main")
        self.assertIn("check: tidy: everything (the changed files cannot be "
                      "worked out", result.stdout)
        self.assertIn("4 of 4 translation units", result.stdout)

    def test_shallow_clone_lints_everything(self):
        self.write("docs/notes.md", "# Notes\n\nMore.\n")
        commit_all(self.root, "documents")
        clone = self.root.parent / "shallow"
        subprocess.run(["git", "clone", "--quiet", "--depth", "1",
                        "--no-single-branch", self.root.as_uri(), str(clone)],
                       check=True, capture_output=True)
        shutil.copytree(self.root / "build", clone / "build")
        # The commands name the files of the first project; point them at
        # the clone's.
        database = clone / "build/compile_commands.json"
        database.write_text(
            database.read_text(encoding="utf-8").replace(
                str(self.root), str(clone)), encoding="utf-8")
        result = self.run_check("--tidy-base", "origin/main", root=clone)
        self.assertIn("the clone is shallow", result.stdout)
        self.assertIn("4 of 4 translation units", result.stdout)

    def test_dependency_error_lints_everything(self):
        self.write("libs/demo/src/four.cpp", '#include "missing.h"\n')
        result = self.run_check("--tidy-base", "main")
        self.assertIn("check: tidy: everything (the changed files cannot be "
                      "worked out", result.stdout)
        self.assertIn("4 of 4 translation units", result.stdout)
        self.assertNotEqual(result.returncode, EXIT_CLEAN)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ("compiler", "clang-format", "clang-tidy", "clang-query"):
        parser.add_argument(f"--{tool}", required=True)
    options, remaining = parser.parse_known_args()
    COMPILER.append(options.compiler)
    TOOLS.extend([
        "--clang-format", options.clang_format,
        "--clang-tidy", options.clang_tidy,
        "--clang-query", options.clang_query,
    ])
    unittest.main(argv=sys.argv[:1] + remaining, verbosity=2)


if __name__ == "__main__":
    main()
