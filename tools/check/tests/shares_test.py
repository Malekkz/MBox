#!/usr/bin/env python3
"""Self-test of the split of the clang-tidy lint across jobs (D-68).

Tests lint_shares.split and lint_shares.verify on made-up lists, and the
split of the translation units of the source tree, found as the check target
finds them.

Usage: shares_test.py --source-dir DIR [--exclude DIR ...]
"""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR.parent))

import lint_shares  # noqa: E402
import scada_check  # noqa: E402

VERIFY = TESTS_DIR.parent / "lint_shares.py"

# Set from the command line before the tests run.
BUILD = {}


def names(count):
    return [f"libs/demo/src/unit{index:03}.cpp" for index in range(count)]


class Split(unittest.TestCase):
    def assert_complete(self, units, shares, split_from):
        parts = lint_shares.split(units, shares, split_from)
        self.assertEqual(len(parts), shares)
        joined = [unit for part in parts for unit in part]
        # Every unit once: no unit lost, none twice.
        self.assertEqual(sorted(joined), sorted(units))
        self.assertEqual(len(joined), len(set(joined)))
        for part in parts:
            self.assertEqual(list(part), sorted(part))
        return parts

    def test_complete_for_many_sizes(self):
        for count in range(0, 60):
            for shares in range(1, 9):
                for split_from in (1, 2, 5, 40):
                    with self.subTest(count=count, shares=shares,
                                      split_from=split_from):
                        self.assert_complete(names(count), shares,
                                             split_from)

    def test_not_a_multiple_of_the_shares(self):
        parts = self.assert_complete(names(10), 4, 1)
        self.assertEqual([len(part) for part in parts], [3, 3, 2, 2])

    def test_fewer_units_than_shares(self):
        parts = self.assert_complete(names(2), 4, 1)
        self.assertEqual([len(part) for part in parts], [1, 1, 0, 0])

    def test_no_units(self):
        self.assertEqual(lint_shares.split([], 4, 1), ((), (), (), ()))

    def test_small_set_is_not_split(self):
        parts = self.assert_complete(names(39), 4, 40)
        self.assertEqual([len(part) for part in parts], [39, 0, 0, 0])
        parts = self.assert_complete(names(40), 4, 40)
        self.assertEqual([len(part) for part in parts], [10, 10, 10, 10])

    def test_deterministic_whatever_the_order(self):
        units = names(45)
        expected = lint_shares.split(units, 4)
        self.assertEqual(lint_shares.split(list(reversed(units)), 4),
                         expected)
        self.assertEqual(lint_shares.split(units[7:] + units[:7], 4),
                         expected)

    def test_round_robin_balances_the_count(self):
        parts = lint_shares.split(names(165), 4)
        self.assertEqual([len(part) for part in parts], [42, 41, 41, 41])
        self.assertEqual(parts[0][:2], (names(165)[0], names(165)[4]))

    def test_duplicate_unit_is_an_error(self):
        with self.assertRaises(lint_shares.ShareError):
            lint_shares.split(names(3) + names(1), 2, 1)

    def test_parse_share(self):
        self.assertEqual(lint_shares.parse_share("2/4"), (2, 4))
        self.assertEqual(lint_shares.parse_share("1/1"), (1, 1))
        for text in ("0/4", "5/4", "4", "a/4", "1/0", "-1/4", "1/2/3", ""):
            with self.subTest(text=text), \
                    self.assertRaises(lint_shares.ShareError):
                lint_shares.parse_share(text)


class BuildUnits(unittest.TestCase):
    """The split of the translation units of the build."""

    @classmethod
    def setUpClass(cls):
        source_dir = BUILD["source_dir"]
        excluded = [(source_dir / tree).resolve()
                    for tree in BUILD["exclude"]]
        files = scada_check.discover_sources(source_dir, excluded)
        cls.units = [
            path.relative_to(source_dir).as_posix() for path in files
            if path.name.endswith(scada_check.UNIT_SUFFIXES)]

    # Whether each unit is part of the build is the check's own test: it
    # refuses a unit without a compile command (scada_check.select_files).
    # Not every preset builds every unit, so this test does not ask.
    def test_the_build_has_units(self):
        self.assertGreater(len(self.units), 0)

    def test_split_of_the_build_is_complete(self):
        count = len(self.units)
        for shares in range(1, 13):
            # The threshold of the pipeline, every unit split, and a
            # threshold above the number of units.
            for split_from in (lint_shares.SPLIT_FROM, 1, count + 1):
                with self.subTest(shares=shares, split_from=split_from):
                    parts = lint_shares.split(self.units, shares, split_from)
                    joined = [unit for part in parts for unit in part]
                    self.assertEqual(sorted(joined), sorted(self.units))
                    self.assertEqual(len(joined), len(set(joined)))
                    self.assertEqual(
                        lint_shares.split(list(reversed(self.units)),
                                          shares, split_from), parts)

    def test_shares_of_the_build_differ_by_one_unit_at_most(self):
        if len(self.units) < lint_shares.SPLIT_FROM:
            self.skipTest("the build has fewer units than SPLIT_FROM")
        sizes = [len(part) for part in lint_shares.split(self.units, 4)]
        self.assertLessEqual(max(sizes) - min(sizes), 1)


def records_of(units, shares, split_from=1, mode="everything"):
    parts = lint_shares.split(units, shares, split_from)
    return [lint_shares.make_record(number, shares, split_from, mode,
                                    "no base revision given", units,
                                    parts[number - 1], clean=True)
            for number in range(1, shares + 1)]


class Verify(unittest.TestCase):
    def assert_fails(self, records, text):
        with self.assertRaises(lint_shares.ShareError) as caught:
            lint_shares.verify(records)
        self.assertIn(text, str(caught.exception))

    def test_complete_records_pass_and_give_the_totals(self):
        lines = lint_shares.verify(records_of(names(10), 4))
        self.assertIn("10 translation units in 4 shares: 3 + 3 + 2 + 2 = "
                      "10, each linted exactly once", lines[1])

    def test_small_set_in_share_one(self):
        lines = lint_shares.verify(records_of(names(3), 4, split_from=40,
                                              mode="changed files"))
        self.assertIn("3 + 0 + 0 + 0 = 3", lines[1])

    def test_no_records(self):
        self.assert_fails([], "no record")

    def test_missing_share(self):
        self.assert_fails(records_of(names(10), 4)[:3],
                          "expected the records of shares 1 to 4")

    def test_share_twice(self):
        records = records_of(names(10), 4)
        self.assert_fails(records + [records[0]],
                          "expected the records of shares 1 to 4")

    def test_share_with_findings(self):
        records = records_of(names(10), 4)
        records[3]["clean"] = False
        self.assert_fails(records, "share 4 of 4 has findings")

    def test_share_that_linted_less(self):
        records = records_of(names(10), 4)
        records[3]["linted"] = records[3]["linted"][:-1]
        self.assert_fails(records, "share 4 of 4 did not lint its part")

    def test_shares_that_disagree_on_the_selection(self):
        records = records_of(names(10), 4)
        records[1]["selected"] = names(9)
        self.assert_fails(records, "the records differ in 'selected'")

    def test_shares_that_disagree_on_the_number(self):
        records = records_of(names(10), 4)
        records[1]["shares"] = 3
        self.assert_fails(records, "the records differ in 'shares'")

    def test_unknown_format(self):
        records = records_of(names(10), 2)
        records[0]["format"] = 99
        self.assert_fails(records, "format")

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for record in records_of(names(10), 4):
                path = pathlib.Path(directory) / f"{record['share']}.json"
                path.write_text(json.dumps(record), encoding="utf-8")
                paths.append(str(path))
            result = subprocess.run(
                [sys.executable, str(VERIFY), "verify"] + paths,
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("3 + 3 + 2 + 2 = 10", result.stdout)
            result = subprocess.run(
                [sys.executable, str(VERIFY), "verify"] + paths[:-1],
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn("the shares are not complete", result.stderr)
            result = subprocess.run(
                [sys.executable, str(VERIFY), "verify"],
                capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--exclude", action="append", default=[])
    options, remaining = parser.parse_known_args()
    BUILD.update(source_dir=options.source_dir.resolve(),
                 exclude=options.exclude)
    unittest.main(argv=sys.argv[:1] + remaining, verbosity=2)


if __name__ == "__main__":
    main()
