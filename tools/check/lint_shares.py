#!/usr/bin/env python3
"""The split of the clang-tidy lint across parallel jobs (decision D-68).

The pipeline runs the `tidy` check as N jobs, the shares. Each share works
out the same selection of translation units (lint_selection.py) and lints
its part of it (`scada_check.py --tidy-share K/N`):

- the units are sorted by their path relative to the source directory, and
  share K takes every N-th unit from the K-th on (a round-robin);
- a selection of fewer than SPLIT_FROM units is not split: share 1 takes all
  of it and the other shares lint nothing.

Each share writes a record of what it linted (`--tidy-record`). The job
`check` runs `lint_shares.py verify` on all records: it passes only if
every share 1 to N wrote a clean record, all records hold the same
selection, and together they lint every selected unit exactly once.

Exit status of `verify`: 0 complete and clean, 1 not.
"""

import argparse
import json
import pathlib
import sys

# A selection of fewer units is linted by one share. 40 units take about as
# long as one share of a full lint (D-68).
SPLIT_FROM = 40

RECORD_FORMAT = 1


class ShareError(Exception):
    """A share specification or a record is not valid."""


def parse_share(text):
    """Parses "K/N" into (K, N), 1 <= K <= N."""
    parts = text.split("/")
    if len(parts) != 2 or not all(part.isdigit() for part in parts):
        raise ShareError(f"'{text}' is not a share; expected K/N, for "
                         "example 2/4")
    share, shares = int(parts[0]), int(parts[1])
    if not 1 <= share <= shares:
        raise ShareError(f"share '{text}': K must be from 1 to N")
    return share, shares


def split(units, shares, split_from=SPLIT_FROM):
    """Splits `units` (relative path strings) into `shares` sorted tuples.

    Every unit is in exactly one tuple, whatever the order of `units`. A
    list shorter than `split_from` is all in the first tuple."""
    if shares < 1:
        raise ShareError("the number of shares must be at least 1")
    ordered = sorted(units)
    if len(set(ordered)) != len(ordered):
        raise ShareError("the list of units holds a unit twice")
    if len(ordered) < split_from:
        return (tuple(ordered),) + ((),) * (shares - 1)
    return tuple(tuple(ordered[index::shares]) for index in range(shares))


def make_record(share, shares, split_from, mode, reason, selected, linted,
                clean):
    return {
        "format": RECORD_FORMAT,
        "share": share,
        "shares": shares,
        "split_from": split_from,
        "mode": mode,
        "reason": reason,
        "selected": sorted(selected),
        "linted": list(linted),
        "clean": clean,
    }


def verify(records):
    """Checks that `records` are the clean records of one complete split.

    Returns the lines of the summary; raises ShareError on any gap."""
    if not records:
        raise ShareError("no record of any share")
    first = records[0]
    for record in records:
        if not isinstance(record, dict) or \
                record.get("format") != RECORD_FORMAT:
            raise ShareError("a record is not of format "
                             f"{RECORD_FORMAT}")
    shares = first["shares"]
    for key in ("shares", "split_from", "mode", "selected"):
        if any(record.get(key) != first.get(key) for record in records):
            raise ShareError(f"the records differ in '{key}'")
    numbers = sorted(record["share"] for record in records)
    if numbers != list(range(1, shares + 1)):
        raise ShareError(f"expected the records of shares 1 to {shares}, "
                         f"found {numbers}")
    selected = first["selected"]
    expected = split(selected, shares, first["split_from"])
    linted = []
    for record in sorted(records, key=lambda item: item["share"]):
        number = record["share"]
        if not record.get("clean"):
            raise ShareError(f"share {number} of {shares} has findings or "
                             "did not finish")
        if tuple(record["linted"]) != expected[number - 1]:
            raise ShareError(f"share {number} of {shares} did not lint its "
                             "part of the selection")
        linted += record["linted"]
    if sorted(linted) != sorted(selected) or len(set(linted)) != len(linted):
        raise ShareError("the shares do not lint every selected unit "
                         "exactly once")
    counts = " + ".join(str(len(record["linted"])) for record in
                        sorted(records, key=lambda item: item["share"]))
    return [
        f"check: tidy: {first['mode']} ({first['reason']})",
        f"check: tidy: {len(selected)} translation units in {shares} "
        f"shares: {counts} = {len(linted)}, each linted exactly once, no "
        "findings",
    ]


def read_records(paths):
    records = []
    for path in paths:
        try:
            with open(path, encoding="utf-8") as stream:
                records.append(json.load(stream))
        except (OSError, ValueError) as error:
            raise ShareError(f"cannot read the record '{path}': {error}") \
                from error
    return records


def main(arguments):
    parser = argparse.ArgumentParser(
        description="Checks the records of the shares of the clang-tidy "
        "lint. Exit status: 0 complete and clean, 1 not.")
    subcommands = parser.add_subparsers(dest="command", required=True)
    verify_parser = subcommands.add_parser(
        "verify", help="check that the records cover the selection exactly "
        "once, without findings")
    verify_parser.add_argument("records", nargs="*", type=pathlib.Path,
                               help="the record files of all shares")
    options = parser.parse_args(arguments)
    try:
        for line in verify(read_records(options.records)):
            print(line)
    except (ShareError, KeyError, TypeError) as error:
        print(f"check: tidy: the shares are not complete: {error}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
