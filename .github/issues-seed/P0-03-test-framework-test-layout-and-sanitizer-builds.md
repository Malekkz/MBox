title: P0-03 Test framework, test layout and sanitizer builds
labels: phase-0
---

**Depends on:** P0-01

## Scope

- Choose the unit test framework and record the choice in `docs/decisions.md` (licence must be permissive).
- CTest labels `unit`, `integration`, `perf`; presets to run each.
- One sample test of each kind against `libscada-hello`.
- Presets for AddressSanitizer (with leak detection) + UndefinedBehaviorSanitizer and for ThreadSanitizer.
- A fuzz preset (libFuzzer with sanitizers) and one sample fuzz target for a toy parser, with a seed corpus directory layout.

## Acceptance

- [ ] `ctest --preset dev` runs unit and integration tests and reports them separately.
- [ ] Sanitizer presets build and run the tests cleanly.
- [ ] Test fixtures with a deliberate heap overflow, use after free, leak, signed overflow and data race are each caught by the matching preset.
- [ ] The sample fuzz target finds a planted bug in the toy parser within 60 seconds.
- [ ] The integration sample starts a real process and waits on a condition, not a sleep.

## References

CLAUDE.md (Tests); docs/coding-standard.md section 13
