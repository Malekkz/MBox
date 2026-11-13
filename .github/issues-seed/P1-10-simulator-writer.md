title: P1-10 Simulator writer
labels: phase-1
---

**Depends on:** P1-06

## Scope

- `scada-sim`: a configurable number of owner processes writing tags at a set rate with selectable patterns (ramp, sine, random walk, step).
- A self-checking pattern whose fields are derived from one another, so readers can detect a torn read.
- Generates the tag-set file for a given tag count, so tests can ask for "50,000 tags across 10 owners".

## Acceptance

- [ ] Sustains 50,000 updates per second across 10 owners on the x86-64 pipeline machine.
- [ ] Used by the conformance and performance tests.

## References

reference 15.3, 18.2 (phase 1)
