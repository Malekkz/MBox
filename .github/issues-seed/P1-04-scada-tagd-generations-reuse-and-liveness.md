title: P1-04 scada-tagd: generations, reuse and liveness
labels: phase-1
---

**Depends on:** P1-03, P0-05

## Scope

- Create the generation directory with the contract's permissions, write `header`, switch `current` atomically.
- Reuse the current generation when layout version and `config_hash` are unchanged.
- Own `proc.tagd`; check heartbeats every `heartbeat_check_ms`; set and clear `owner_down`.
- Run as a notify service under its own user.

## Acceptance

- [ ] Conformance tests 4 (owner-down part) and 11 pass.
- [ ] `scada-tagd` needs no capability beyond its own user and the `scada` group.

## References

contract sections 3, 10, 11.1
