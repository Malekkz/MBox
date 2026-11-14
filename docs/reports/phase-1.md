# Phase 1 report

The figures that phase 1 is accepted on (contract section 15, reference
section 15). Issue P1-12 completes this report with the benchmarks of every
conformance test.

## Read retry window and `SCADA_E_BUSY` (issue #40)

Decision D-31. Measured on 2026-11-14 by the pipeline job
`arm64-measurements` (run 43606154224, commit `f3d99c7`), `perf` build (GCC
13.3, `-O2`, hardening flags), with `scripts/measure-seqlock.sh <build> 60
600`.

### Hardware

| Runner | Processor | Processors | Clock source |
| --- | --- | --- | --- |
| `ubuntu-24.04-arm` | ARM Neoverse N2 | 2 | `arch_sys_counter` |
| `ubuntu-24.04` | x86-64 (GitHub hosted) | 2 | not recorded |

Both are shared virtual machines, not the target board. With 2 processors
the bench runs 1 writer and 1 reader thread.

### Time of one call

Fastest of 7 batches of 1,000,000 calls, in ns. A write is the whole call,
an upper bound of the time the record is odd.

| Figure | ARM64, `yield` | ARM64, `isb` | x86-64, `pause` |
| --- | --- | --- | --- |
| 64 attempts on a record mid-write | 22 | 665 | 1,587 |
| Scalar write | 8.4 | 9.4 | 4.6 |
| String write, 250 bytes | 113 | 113 | 75 |
| Scalar read, no writer | 17 | 15 | 15 |
| String read, no writer | 105 | 103 | 86 |

### Reads that give up

`scada-tag-seqlock-bench load`, 60 s, 1 reader. Rate as 1 read in N.

| Load | ARM64, `yield` | ARM64, `isb` | x86-64, `pause` |
| --- | --- | --- | --- |
| 1 scalar tag, written without pause | 1 in 4 | 1 in 129 | 1 in 9,582 |
| 1 string tag, written without pause | 1 in 1 | 1 in 1 | 1 in 1 |
| 50,000 scalar tags, 50,000 writes/s | 0 in 3.25 billion | 0 in 3.26 billion | 0 in 2.69 billion |
| 1,000 string tags, 50,000 writes/s | 1 in 80,625 | 1 in 54 million | 1 in 25 million |

No read was torn in any run.

### Reference load

Conformance test 2: 50,000 tags across 10 owners, 50,000 updates per second,
4 readers, real processes and `scada-tagd`, 600 s. Criterion (contract
section 16): fewer than 1 `SCADA_E_BUSY` in 1,000,000 reads.

| | ARM64, `yield` | ARM64, `isb` | x86-64, `pause` |
| --- | --- | --- | --- |
| Updates per second | 49,998 | 49,998 | 49,999 |
| Reads | 33.1 billion | 32.5 billion | 26.1 billion |
| `SCADA_E_BUSY` | 0 | 5 (1 in 6.5 billion) | 0 |

The rate on ARM64 is below the criterion with either instruction. ARM64 uses
`isb` (D-31): it is the one that covers a string write.
