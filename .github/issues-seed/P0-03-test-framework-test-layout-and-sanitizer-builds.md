title: P0-03 Test framework, test layout and sanitizer builds
labels: phase-0
---

**Depends on:** P0-01

## Scope

- Choose the unit test framework and record the choice in `docs/decisions.md` (licence must be permissive).
- CTest labels `unit`, `integration`, `perf`; presets to run each.
- One sample test of each kind against `libscada-hello`.
- Presets for AddressSanitizer + UndefinedBehaviorSanitizer and for ThreadSanitizer.

## Acceptance

- [ ] `ctest --preset dev` runs unit and integration tests and reports them separately.
- [ ] Sanitizer presets build and run the tests cleanly.
- [ ] The integration sample starts a real process and waits on a condition, not a sleep.

## References

CLAUDE.md (Tests)
