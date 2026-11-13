title: P0-01 Repository skeleton and CMake build
labels: phase-0
---

## Scope

- Create the directory layout: `libs/`, `services/`, `tools/`, `tests/`, `cmake/`, `docs/`.
- CMake presets `dev`, `release` and `perf`. C++20, warnings as errors, hardening flags in `release` (reference 13.6).
- One trivial library `libscada-hello` and a thin executable `scada-hello` that uses it, to prove the library-plus-thin-executable pattern.
- Component `README.md` template.

## Acceptance

- [ ] `cmake --preset dev && cmake --build --preset dev` succeeds on x86-64 with no warnings.
- [ ] The same source cross-compiles or builds natively for ARM64.
- [ ] No distribution-specific path or tool appears in any build file.

## References

CLAUDE.md (Code); reference 4.1
