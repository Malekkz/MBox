title: P1-07 Change rings and the wake-up mechanism
labels: phase-1
---

**Depends on:** P1-06

## Scope

- Change entry append on value or quality change; `scada_sub_open`, `scada_sub_next`, `scada_sub_close`.
- Gap detection and resynchronisation as in the contract.
- Experiment: futex wait on a read-only shared mapping on both architectures. Record the result and the chosen mechanism in `docs/decisions.md`, then update contract section 8.4.

## Acceptance

- [ ] Conformance test 9 passes.
- [ ] A blocked consumer wakes within 1 ms of a write at the 99th percentile on x86-64 (report the ARM64 figure).

## References

contract section 8, 16
