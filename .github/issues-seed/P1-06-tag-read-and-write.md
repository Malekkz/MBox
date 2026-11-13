title: P1-06 Tag read and write
labels: phase-1,needs-human-review
---

**Depends on:** P1-05

## Scope

- `scada_tag_resolve`, `scada_tag_info`, `scada_tag_read`, `scada_tag_read_many`, `scada_tag_write`, and the string variants.
- Sequence-lock protocol exactly as in the contract, including the bounded retry and the owner-state overlay.
- Mark the real-time functions `SCADA_RT`; add test-build instrumentation that detects system calls and allocations inside them.

## Acceptance

- [ ] Conformance tests 2, 3, 4, 6 and 13 pass.
- [ ] ThreadSanitizer reports nothing other than documented, annotated sequence-lock reads.

## References

contract sections 6, 7, 13

## Review

This issue needs review by a person before it is closed.
