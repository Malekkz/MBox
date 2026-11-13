title: P0-02 Formatter, linter and the real-time annotation
labels: phase-0
---

**Depends on:** P0-01

## Scope

- Add `.clang-format` and `.clang-tidy` matching the naming and style rules in `CLAUDE.md`.
- Enable the clang-tidy check groups and the error list of `docs/coding-standard.md` section 13.
- Add the banned-function check for the list in coding standard section 12.
- A `check` build target that fails on any formatting or lint finding.
- Define the `SCADA_RT` annotation macro and a first check that flags calls from an annotated function to a small deny-list (`malloc`, `new`, `printf`, mutex lock).

## Acceptance

- [ ] `cmake --build --preset dev --target check` passes on the skeleton and fails on a deliberately mis-formatted file.
- [ ] A test file with an allocation inside a `SCADA_RT` function is reported.
- [ ] Test files containing `strcpy`, an owning raw pointer, and a narrowing conversion are each rejected.
- [ ] Any clang-tidy check disabled as unworkable has a one-line reason in `.clang-tidy` and an entry in `docs/decisions.md`.

## References

CLAUDE.md (Code, Real-time code); docs/coding-standard.md sections 12 to 14
