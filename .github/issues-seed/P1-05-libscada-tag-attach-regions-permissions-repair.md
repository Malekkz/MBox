title: P1-05 libscada-tag: attach, regions, permissions, repair
labels: phase-1
---

**Depends on:** P1-04

## Scope

- `scada_tag_attach`, `scada_tag_detach`, `scada_tag_heartbeat`, `scada_tag_status`.
- Create own regions exclusively; reuse and repair existing ones; verify layout version, generation, size and file owner uid.
- Map other regions read-only on demand; tolerate missing or not-ready regions and retry in the background.

## Acceptance

- [ ] Conformance tests 5, 7 and 8 pass.
- [ ] A process not listed in the process table, or running as the wrong user, cannot attach.

## References

contract sections 3, 6.3, 11.2, 13
