#!/usr/bin/env bash
# Measures the sequence lock of the tag database on this machine (issue #40,
# decision D-31) and prints the figures as `key=value` lines:
#
# 1. scada-tag-seqlock-bench timings: the time of the 64 read attempts, of
#    one scalar and one string write, and of a read.
# 2. scada-tag-seqlock-bench load: reads that give up, for one scalar and
#    one string tag written as fast as possible (the worst case), for 50,000
#    scalar tags and for 1,000 string tags at 50,000 writes per second.
# 3. Conformance test 2 (the reference load: 50,000 tags, 10 owners, 50,000
#    updates per second, 4 readers, real processes) and its count of
#    SCADA_E_BUSY.
#
# Usage: scripts/measure-seqlock.sh <build directory> <load seconds>
#            <conformance seconds>
#
# The build directory is one configured with the `perf` preset, for example
# `cmake --preset perf -B build/measure -DSCADA_ARM64_PAUSE=isb`, with the
# targets scada-tag-seqlock-bench and scada-tag-access-integration-test
# built. Needs python3 to read the test list and the test report.
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <build directory> <load seconds> <conformance seconds>" >&2
  exit 2
fi
build=$(cd "$1" && pwd)
load_seconds=$2
conformance_seconds=$3

bench="${build}/libs/scada-tag/tests/scada-tag-seqlock-bench"
processors=$(nproc)
# One processor for the writer, the others for readers.
readers=$((processors > 1 ? processors - 1 : 1))

echo "machine=$(uname -m) processors=${processors}"
"${bench}" timings
"${bench}" load scalar 1 0 "${readers}" "${load_seconds}"
"${bench}" load string 1 0 "${readers}" "${load_seconds}"
"${bench}" load scalar 50000 50000 "${readers}" "${load_seconds}"
"${bench}" load string 1000 50000 "${readers}" "${load_seconds}"

# Conformance test 2 runs longer than the time limit CTest gives an
# integration test, so the test program is started directly, with the
# environment CTest would give it.
report="${build}/measure-conformance02.xml"
ctest --test-dir "${build}" --show-only=json-v1 \
    -R '^scada-tag-access-integration-test$' > "${build}/measure-tests.json"
mapfile -t setup < <(python3 -I - "${build}/measure-tests.json" <<'EOF'
import json, sys
test = json.load(open(sys.argv[1]))["tests"][0]
properties = {p["name"]: p["value"] for p in test["properties"]}
print(properties["WORKING_DIRECTORY"])
print(test["command"][-1])
for entry in properties["ENVIRONMENT"]:
    if entry:
        print(entry)
EOF
)
directory=${setup[0]}
program=${setup[1]}
(
  cd "${directory}"
  env "${setup[@]:2}" \
      SCADA_CONFORMANCE_SECONDS="${conformance_seconds}" \
      GTEST_FILTER='*Conformance02*' \
      GTEST_OUTPUT="xml:${report}" \
      "${program}"
)
python3 -I - "${report}" <<'EOF'
import sys
import xml.etree.ElementTree as tree
for case in tree.parse(sys.argv[1]).iter("testcase"):
    # Recorded properties are attributes in older GoogleTest versions and
    # <property> elements in newer ones.
    a = dict(case.attrib)
    for p in case.iter("property"):
        a[p.attrib["name"]] = p.attrib["value"]
    busy, reads = int(a["busy"]), int(a["reads"])
    line = (f"conformance=02 seconds={a['seconds']} "
            f"updates_per_second={a['updates_per_second']} "
            f"reads={reads} busy={busy}")
    if busy:
        line += f" reads_per_busy={reads // busy}"
    print(line)
EOF
