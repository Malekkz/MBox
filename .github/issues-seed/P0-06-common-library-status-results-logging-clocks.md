title: P0-06 Common library: status, results, logging, clocks
labels: phase-0
---

**Depends on:** P0-01, P0-03

## Scope

- `libscada-common` with `Status` and `Result<T>`, structured logging to the systemd journal with a stderr fallback, and helpers for `CLOCK_REALTIME` and `CLOCK_MONOTONIC` in nanoseconds.
- No exceptions across the library boundary.
- Unit tests for each.

## Acceptance

- [ ] All public functions have contract comments.
- [ ] Logging never blocks the caller for more than a bounded time, shown by a test with a stalled sink.
- [ ] Unit tests pass on both architectures.

## References

CLAUDE.md (Code)
