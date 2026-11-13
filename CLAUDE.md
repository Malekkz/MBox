# Project rules

This repository builds an embedded SCADA system. These rules apply to every
change, whoever or whatever makes it.

## Read first

1. `docs/reference-architecture.md` is the design baseline (version 1.0). It
   decides what the system is. Do not contradict it.
2. `docs/contracts/` holds the interface contracts. A contract decides how two
   parts talk to each other. Code must match the contract, never the reverse.
3. `docs/decisions.md` records every choice the reference document left open.
4. `docs/coding-standard.md` is the C++ coding standard. Read it before
   writing or reviewing C++.

If these disagree, stop and ask (see "When to stop and ask").

## How work is organised

- One issue, one branch, one pull request. Keep pull requests small enough to
  review in one sitting.
- Never commit or push to `main` directly. Every change reaches `main` through
  a pull request, and a person merges it. Do not merge your own pull request.
- Never force-push to `main`, and never delete or rewrite a branch you did not
  create. The repository has no enforced branch protection, so these rules are
  the protection.
- Do not merge a pull request whose checks are failing or still running.
- Branch names: use the branch the session assigns. If you choose the name
  yourself, use `phase<N>/<issue-number>-<short-name>`. Either way, the pull
  request title starts with the issue ID (for example `P0-01`) and the
  description closes the issue (`Closes #2`).
- Work on the issue you were given. Do not start later phases, and do not add
  features the issue does not ask for.
- Each service is a library plus a thin executable: all logic lives in the
  library (`lib<name>`), and `main()` only parses arguments, wires things up and
  runs. This keeps code unit-testable and keeps a combined build possible later.

## Definition of done

A pull request is ready only when all of these hold:

- [ ] It builds with no warnings on x86-64 and ARM64.
- [ ] All tests pass on both, including the ones added for this change.
- [ ] Formatter, linter and sanitizer runs are clean.
- [ ] New behaviour has unit tests; anything that crosses a process boundary
      has an integration test.
- [ ] Interface or behaviour changes are reflected in the documentation in the
      same pull request.
- [ ] No contract changed without a version bump and a migration note.
- [ ] Any choice the documents left open is recorded in `docs/decisions.md`.
- [ ] The pull request description lists what was tested and how.

Never mark work as done with failing or skipped tests. If a test cannot pass
yet, say so in the pull request and leave the issue open.

## Code

This section is the short version. The full rules, including ownership,
bounds, integer, concurrency and shared-memory rules, are in
`docs/coding-standard.md`, and they are binding.

- C++20, built with CMake. No compiler extensions, no architecture-specific
  code outside the I/O HAL.
- Warnings are errors. The warning, hardening and library-assertion flags are
  listed in section 2 of the coding standard.
- No distribution-specific paths, tools or package managers anywhere in code or
  build scripts. Install through the CMake install tree only.
- Errors are returned as values (`Status`, `Result<T>`). No exceptions across
  library boundaries, and none at all in real-time code.
- No raw `new`/`delete`. No global mutable state outside clearly named
  singletons for process-wide resources.
- All external input is untrusted: protocol frames, API requests, configuration
  files, imported files. Validate lengths, ranges and types before use.
- Names: `snake_case` for functions and variables, `PascalCase` for types,
  `UPPER_CASE` for constants and enumerators. Process and library names as in
  the reference document.
- Comments explain why, not what. Every public function in a library header has
  a short comment stating its contract.

### Real-time code

Code that runs inside a control task, the I/O scan, or the tag library's
real-time functions (`read`, `read_many`, `write`, `post_request`) must not:

- allocate or free memory,
- block, sleep, or take a lock that a non-real-time thread can hold,
- make system calls, log, or do file or network I/O.

Mark such functions with the `SCADA_RT` annotation so the checks can find them.

## Tests

- Unit tests live next to the code in `tests/` of each component and run in
  seconds.
- Integration tests start real processes against simulated devices. Every
  driver ships with a simulator for its protocol.
- Performance tests check the targets in section 15 of the reference document.
  A regression against a target fails the build.
- Tests must be deterministic. No sleeps to "wait for things"; wait on a
  condition with a timeout.
- A bug fix comes with a test that fails without the fix.
- Every pull request passes the address, undefined-behaviour and thread
  sanitizer runs. Every parser of outside input ships with a fuzz target.

## Safety and security rules

Changes to any of the following need review by a person before merging, and
must say so at the top of the pull request:

- command handling, select-before-operate, interlocks, control authority,
- fail-safe output behaviour, shutdown and restart behaviour,
- authentication, permissions, audit, signing, secrets,
- the shared-memory layout or its access permissions.

Further rules:

- "No audit, no action": a command that cannot be journalled is rejected.
- Never weaken a check to make a test pass. Fix the code or fix the test.
- Never commit secrets, keys, real site configuration or real addresses.
- Never add a dependency without recording its licence in the licence report.
  GPL-licensed code is not allowed anywhere until the licensing decision in
  section 16 of the reference document is made.

## Documentation

- User-visible behaviour is documented where users look: CLI help text, the
  OpenAPI document, configuration schema descriptions.
- Each component has a short `README.md`: what it does, how to run it, how to
  test it.
- Keep documents short and specific. Numbers carry units. No marketing words.

## When to stop and ask

Stop, write down the question in the issue, and wait for an answer when:

- the reference document, a contract and the decisions log disagree,
- something is unspecified and affects safety, security or an external
  interface,
- a contract would have to change,
- a performance target cannot be met,
- the task turns out to be much larger than the issue describes.

For anything else that is unspecified, choose the simplest option that fits the
reference document, record it in `docs/decisions.md`, and continue.

## Commands

These are created in phase 0; keep this list current.

```
cmake --preset dev          # configure
cmake --build --preset dev  # build
cmake --build --preset dev --target check   # formatter, linter, banned-function and SCADA_RT checks
ctest --preset dev          # unit and integration tests
ctest --preset unit         # unit tests only (dev build)
ctest --preset integration  # integration tests only (dev build)
ctest --preset perf         # performance tests
cmake --install build/dev --prefix stage   # staged install tree
```

Replace `dev` with `release` for the build with the release flags, or with `dev-arm64` or
`release-arm64` to cross-compile for ARM64 (see `README.md`, "Building").

Replace `dev` with `asan-ubsan`, `tsan` or `fuzz` for the sanitizer and fuzz
builds (see `README.md`, "Testing"). `perf`, `asan-ubsan`, `tsan` and `fuzz`
each need their own configure and build step first.
