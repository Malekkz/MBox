title: P1-02 Layout structures and static checks
labels: phase-1
---

**Depends on:** P1-01, P0-06

## Scope

- Headers defining every structure in the contract, with `static_assert` on size, alignment and field offsets.
- Enumerations, handle packing and unpacking, FNV-1a name hash.
- Unit tests, including a check that all atomics used are lock-free on both architectures.

## Acceptance

- [ ] Conformance test 1 of the contract passes on x86-64 and ARM64.

## References

contract sections 4 to 10
