title: P1-12 Conformance suite, benchmarks and performance gate
labels: phase-1
---

**Depends on:** P1-09, P1-10, P1-11

## Scope

- All 14 conformance tests of the contract automated in the pipeline on both architectures.
- Benchmark report: throughput, and median and 99.9th percentile latency for `read`, `read_many` of 500 tags and `write`, on x86-64 and ARM64.
- A `perf` gate that fails the build on regression beyond a stated tolerance.
- Measure per-process memory of each phase 1 process.

## Acceptance

- [ ] Every conformance test passes on both architectures.
- [ ] The report is committed under `docs/reports/phase-1.md` with the hardware it ran on.
- [ ] 50,000 tags, 50,000 updates per second sustained for 10 minutes with 4 readers.

## References

contract section 15; reference 15.2
