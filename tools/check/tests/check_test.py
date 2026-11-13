#!/usr/bin/env python3
"""Tests of scada_check.py. They run the real tools on the fixture files.

Usage: check_test.py --clang-format PATH --clang-tidy PATH --clang-query PATH
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
FIXTURES = TESTS_DIR / "fixtures"
SCRIPT = TESTS_DIR.parent / "scada_check.py"
SOURCE_DIR = TESTS_DIR.parents[2]

EXIT_CLEAN = 0
EXIT_FINDINGS = 1
EXIT_CANNOT_RUN = 2

# Set from the command line before the tests run.
TOOLS = []


def run_check(*arguments, source_dir=SOURCE_DIR):
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--source-dir", str(source_dir)]
        + TOOLS + [str(argument) for argument in arguments],
        capture_output=True, text=True, check=False)


def check_fixture(name, *checks):
    """Runs the named checks on one fixture, compiled on its own."""
    only = [f"--only={check}" for check in checks]
    return run_check(
        *only, "--compile-flag=-std=c++20",
        f"--compile-flag=-I{SOURCE_DIR / 'libs/scada-common/include'}",
        FIXTURES / name)


def expected_lines(name, check):
    """Line numbers of the fixture marked `// expect: <check>`."""
    text = (FIXTURES / name).read_text(encoding="utf-8")
    return {
        number
        for number, line in enumerate(text.splitlines(), start=1)
        if line.rstrip().endswith(f"// expect: {check}")
    }


def reported_lines(result, name, check):
    """Line numbers of the fixture that the output reports for `check`."""
    pattern = re.compile(
        rf"^{re.escape(str(FIXTURES / name))}:(\d+):\d+: error: .* "
        rf"\[{re.escape(check)}\]$", re.MULTILINE)
    return {int(number) for number in pattern.findall(result.stdout)}


class CleanFile(unittest.TestCase):
    def test_every_check_passes(self):
        result = check_fixture("clean.cpp")
        self.assertEqual(result.returncode, EXIT_CLEAN,
                         result.stdout + result.stderr)
        self.assertIn("no findings", result.stdout)


class FormatCheck(unittest.TestCase):
    def test_misformatted_file_fails(self):
        result = check_fixture("misformatted.cpp", "format")
        self.assertEqual(result.returncode, EXIT_FINDINGS, result.stderr)
        self.assertIn("misformatted.cpp", result.stdout)
        self.assertIn("-Wclang-format-violations", result.stdout)

    def test_misformatted_file_is_found_by_search(self):
        """Without file arguments the source roots are searched, and an
        excluded directory is left out."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shutil.copy(SOURCE_DIR / ".clang-format", root)
            for tree in ("libs/a/src", "libs/a/fixtures"):
                (root / tree).mkdir(parents=True)
                shutil.copy(FIXTURES / "misformatted.cpp", root / tree)

            result = run_check("--only=format", source_dir=root)
            self.assertEqual(result.returncode, EXIT_FINDINGS, result.stderr)
            self.assertIn("src/misformatted.cpp", result.stdout)
            self.assertIn("fixtures/misformatted.cpp", result.stdout)

            result = run_check("--only=format", "--exclude=libs/a/fixtures",
                               source_dir=root)
            self.assertEqual(result.returncode, EXIT_FINDINGS, result.stderr)
            self.assertIn("src/misformatted.cpp", result.stdout)
            self.assertNotIn("fixtures/misformatted.cpp", result.stdout)

            result = run_check("--only=format", "--exclude=libs/a",
                               source_dir=root)
            self.assertEqual(result.returncode, EXIT_CLEAN, result.stderr)


class TidyCheck(unittest.TestCase):
    def assert_rejected(self, name, tidy_check):
        result = check_fixture(name, "tidy")
        self.assertEqual(result.returncode, EXIT_FINDINGS, result.stderr)
        self.assertRegex(result.stdout,
                         rf"{re.escape(name)}:\d+:\d+: error: .*{tidy_check}")

    def test_owning_raw_pointer_is_rejected(self):
        self.assert_rejected("owning_raw_pointer.cpp",
                             "cppcoreguidelines-owning-memory")

    def test_narrowing_conversion_is_rejected(self):
        self.assert_rejected("narrowing.cpp",
                             "cppcoreguidelines-narrowing-conversions")

    def test_finding_outside_the_error_groups_is_rejected(self):
        """.clang-tidy makes five groups errors; the check fails on the
        others too. The fixture breaks only the naming rules."""
        result = check_fixture("naming.cpp", "tidy")
        self.assertEqual(result.returncode, EXIT_FINDINGS, result.stderr)
        self.assertNotRegex(result.stdout,
                            r"error: .*\[(?!readability-identifier-naming)")
        for name in ("BadFunction", "BadParameter", "bad_constant",
                     "bad_type", "BadMember", "missing_suffix", "bad_value"):
            self.assertRegex(
                result.stdout,
                rf"invalid case style for .* '{name}' "
                r"\[readability-identifier-naming")


class BannedCheck(unittest.TestCase):
    def assert_reports_marked_lines(self, name):
        result = check_fixture(name, "banned")
        self.assertEqual(result.returncode, EXIT_FINDINGS, result.stderr)
        expected = expected_lines(name, "scada-banned")
        self.assertTrue(expected, "fixture marks no line")
        self.assertEqual(reported_lines(result, name, "scada-banned"),
                         expected)

    def test_strcpy_is_rejected(self):
        self.assert_reports_marked_lines("banned_strcpy.cpp")
        result = check_fixture("banned_strcpy.cpp", "banned")
        self.assertIn("std::strcpy(destination, source);", result.stdout)

    def test_owning_raw_pointer_is_rejected(self):
        self.assert_reports_marked_lines("owning_raw_pointer.cpp")

    def test_every_entry_of_the_banned_list_is_rejected(self):
        self.assert_reports_marked_lines("banned_all.cpp")

    def test_using_namespace_in_a_header_is_rejected(self):
        result = check_fixture("banned_all.cpp", "banned")
        self.assertEqual(
            reported_lines(result, "banned_using.h", "scada-banned"),
            expected_lines("banned_using.h", "scada-banned"))

    def test_only_a_suppression_naming_the_check_is_honoured(self):
        self.assert_reports_marked_lines("suppressed.cpp")


class RealTimeCheck(unittest.TestCase):
    def test_allocation_lock_and_output_are_reported(self):
        name = "rt_violations.cpp"
        result = check_fixture(name, "rt")
        self.assertEqual(result.returncode, EXIT_FINDINGS, result.stderr)
        self.assertEqual(reported_lines(result, name, "scada-rt"),
                         expected_lines(name, "scada-rt"))
        self.assertIn("allocation", result.stdout)

    def test_unannotated_code_is_not_reported(self):
        result = check_fixture("banned_all.cpp", "rt")
        self.assertEqual(result.returncode, EXIT_CLEAN,
                         result.stdout + result.stderr)


class Failures(unittest.TestCase):
    """The checks must not pass when they could not run."""

    def test_file_that_does_not_compile(self):
        with tempfile.TemporaryDirectory(dir=FIXTURES) as directory:
            broken = pathlib.Path(directory) / "broken.cpp"
            broken.write_text("int broken( {\n", encoding="utf-8")
            for check in ("tidy", "banned", "rt"):
                result = run_check(f"--only={check}",
                                   "--compile-flag=-std=c++20", broken)
                self.assertNotEqual(result.returncode, EXIT_CLEAN, check)

    def test_missing_file(self):
        result = run_check("--only=format", FIXTURES / "no_such_file.cpp")
        self.assertEqual(result.returncode, EXIT_CANNOT_RUN)
        self.assertIn("no such file", result.stderr)

    def test_missing_tool(self):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--source-dir", str(SOURCE_DIR),
             "--only=format", "--clang-format", "/nonexistent/clang-format",
             str(FIXTURES / "clean.cpp")],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, EXIT_CANNOT_RUN)

    def test_file_outside_the_build(self):
        """A source file that the build does not compile cannot be analysed;
        that is an error, not a silent pass."""
        with tempfile.TemporaryDirectory() as directory:
            build = pathlib.Path(directory)
            (build / "compile_commands.json").write_text(
                "[]", encoding="utf-8")
            result = run_check("--only=banned", "--build-dir", build,
                               FIXTURES / "clean.cpp")
            self.assertEqual(result.returncode, EXIT_CANNOT_RUN)
            self.assertIn("not part of the build", result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ("clang-format", "clang-tidy", "clang-query"):
        parser.add_argument(f"--{tool}", required=True)
    options, remaining = parser.parse_known_args()
    TOOLS.extend([
        "--clang-format", options.clang_format,
        "--clang-tidy", options.clang_tidy,
        "--clang-query", options.clang_query,
    ])
    unittest.main(argv=sys.argv[:1] + remaining, verbosity=2)


if __name__ == "__main__":
    main()
