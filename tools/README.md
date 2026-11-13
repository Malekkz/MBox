# tools

Developer and operator tools, one subdirectory each, built as a library plus a
thin executable like the services.

| Directory | Content |
| --- | --- |
| `check/` | The `check` build target: formatter, linter, banned-function and `SCADA_RT` checks. A script of the build machine, not an installed tool. |

The first installed tool is `scadactl` (phase 1).
