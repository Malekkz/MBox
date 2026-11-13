title: P0-04 Pipeline: build and test on x86-64 and ARM64
labels: phase-0
---

**Depends on:** P0-02, P0-03

## Scope

- A GitHub Actions workflow that, for every pull request, builds and tests on x86-64 and ARM64, runs `check`, and runs the sanitizer presets on x86-64.
- Decide how ARM64 is run (native runner or emulation) and record it in `docs/decisions.md`, with the measured job time.
- Upload the staged install tree as a build artifact once P0-05 exists.

## Acceptance

- [ ] A pull request with a failing test, a warning, or a formatting error cannot pass.
- [ ] Both architectures appear as separate required checks.
- [ ] Total pipeline time is reported in the pull request that adds it.

## References

reference 18.1; CLAUDE.md (Definition of done)
