title: P1-11 scadactl: status and tag commands on shared memory
labels: phase-1
---

**Depends on:** P1-07

## Scope

- `scadactl status`: generation, processes with heartbeat state, owners with `owner_down`.
- `scadactl tag list [pattern]`, `tag get <name>`, `tag watch <pattern>` using change rings.
- Human-readable output by default and `--json`.
- Read-only: this is the seed of the later emergency direct mode. No write commands.

## Acceptance

- [ ] `tag watch` shows changes from `scada-sim` with value, quality, flags and age.
- [ ] Bad and stale quality are clearly marked in both output forms.
- [ ] Help text documents every command.

## References

reference 11.5, 11.6
