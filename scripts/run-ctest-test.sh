#!/usr/bin/env bash
# Runs one CTest test of a build directory directly, with the command, the
# working directory and the environment CTest would give it, but without
# its time limit: for runs longer than CTest allows an integration test,
# such as the 10 minutes of conformance tests 2 and 26 (issue P1-12,
# decision D-57). Extra `NAME=value` arguments are added to the environment.
#
# Usage: scripts/run-ctest-test.sh <build directory> <test name>
#            [NAME=value ...]
#
# Supports a test whose command is the test program alone, without
# arguments and without CROSSCOMPILING_EMULATOR (a native build): it fails,
# saying so, for any other command, rather than run something else.
#
# Needs python3 to read the test list.
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <build directory> <test name> [NAME=value ...]" >&2
  exit 2
fi
build=$(cd "$1" && pwd)
name=$2
shift 2

list=$(mktemp)
trap 'rm -f "${list}"' EXIT
ctest --test-dir "${build}" --show-only=json-v1 -R "^${name}\$" > "${list}"
mapfile -t setup < <(python3 -I - "${list}" <<'PYTHON'
import json, sys
tests = json.load(open(sys.argv[1]))["tests"]
if len(tests) != 1:
    sys.exit(f"expected one test, found {len(tests)}")
test = tests[0]
properties = {p["name"]: p["value"] for p in test.get("properties", [])}
command = test.get("command", [])
if len(command) != 1 or "CROSSCOMPILING_EMULATOR" in properties:
    sys.exit(f"only a command of one program is supported: {command}")
print(properties["WORKING_DIRECTORY"])
print(command[0])
for entry in properties.get("ENVIRONMENT", []):
    if entry:
        print(entry)
PYTHON
)
if [[ ${#setup[@]} -lt 2 ]]; then
  echo "$0: cannot run ${name} directly (see above)" >&2
  exit 1
fi
cd "${setup[0]}"
exec env "${setup[@]:2}" "$@" "${setup[1]}"
