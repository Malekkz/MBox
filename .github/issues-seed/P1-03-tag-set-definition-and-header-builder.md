title: P1-03 Tag-set definition and header builder
labels: phase-1
---

**Depends on:** P1-02

## Scope

- A temporary, minimal tag-set file format (JSON) listing processes, owners, queues and tags. It is replaced by real configuration in phase 2; keep it behind one interface.
- Compute `config_hash` from the tag-set definition, independent of file ordering.
- Build the complete `header` region image in memory: tables, sorted directory, name pool.

## Acceptance

- [ ] The same tag set in a different order yields the same `config_hash` and the same directory.
- [ ] Duplicate names, unknown owners and limits from contract section 14 are rejected with clear errors.
- [ ] A 50,000-tag header builds in under 1 second.

## References

contract section 5, 14
