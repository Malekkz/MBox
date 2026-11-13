# MBox

MBox is a SCADA node that runs on one embedded Linux PC per site: it collects
and logs data, controls field devices, and serves a web HMI and a local CLI.

Status: design baseline frozen (version 1.0). No code yet. Phase 0 is next.

## Where things are

| Path | Content |
| --- | --- |
| `docs/reference-architecture.md` | The design baseline. Start here. |
| `docs/contracts/` | Interface contracts between the parts. |
| `docs/decisions.md` | Log of decisions taken during the build. |
| `docs/coding-standard.md` | C++ coding standard, focused on memory safety. |
| `CLAUDE.md` | Rules for every change: code, tests, documentation, review. |
| `.github/issues-seed/` | The phase 0 and phase 1 issues, ready to create. |
| `scripts/create-issues.sh` | Creates those issues on GitHub. |

## Build order

The build follows the phases in section 18 of the reference document. Phases 0
to 2 are built one issue at a time, because everything else depends on them.
From phase 3, parts that depend only on frozen contracts can be built in
parallel.

## First steps

1. Review and merge the pull request that adds this starting set.
2. Protect the `main` branch: require passing checks and one review.
3. Start with issue #2 (P0-01).

The phase 0 and 1 issues (#2 to #21) already exist on GitHub. Do not run
`scripts/create-issues.sh` on this repository again, or they will be
duplicated; the script is kept for creating later phases' issues the same way.
