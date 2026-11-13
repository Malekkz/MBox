title: P1-09 Generation change and retained values
labels: phase-1
---

**Depends on:** P1-07, P1-08

## Scope

- `RETIRING` detection, detach and reattach, handle re-resolution, removal of the old generation.
- Retain file writer in `scada-tagd` (atomic write) and restore on creation of a new `tags` region.

## Acceptance

- [ ] Conformance test 12 passes.
- [ ] Killing power (simulated by `SIGKILL` of all processes at random points) never leaves an unreadable retain file.

## References

contract sections 11.3, 12
