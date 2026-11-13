title: P0-07 Dependency list and licence report
labels: phase-0
---

**Depends on:** P0-01

## Scope

- A machine-readable list of third-party dependencies with version and licence.
- A build step that fails when a dependency is missing from the list or its licence is not on the allow-list. GPL is not on the list.
- Generate a software bill of materials as a build artifact.

## Acceptance

- [ ] Adding an unlisted dependency fails the build with a clear message.
- [ ] The report and bill of materials are attached to pipeline runs.

## References

reference 13.6, 16.3
