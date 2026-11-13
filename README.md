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
| `CLAUDE.md` | Rules for every change: code, tests, documentation, review. |
| `.github/issues-seed/` | The phase 0 and phase 1 issues, ready to create. |
| `scripts/create-issues.sh` | Creates those issues on GitHub. |

## Build order

The build follows the phases in section 18 of the reference document. Phases 0
to 2 are built one issue at a time, because everything else depends on them.
From phase 3, parts that depend only on frozen contracts can be built in
parallel.

## First steps

1. Create the GitHub repository and push this tree.
2. Protect the `main` branch: require passing checks and one review.
3. Run `scripts/create-issues.sh` to create the phase 0 and 1 issues.
4. Start with issue P0-01.
