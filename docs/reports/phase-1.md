# Phase 1 report

The figures that phase 1 is accepted on (contract section 15, reference
section 15). Issue P1-12 (#20) adds the conformance suite, the benchmarks
and the performance gate (decision D-57); its figures follow the section of
issue #40. Issue P1-13 (#21) adds a second measurement run with the tests of
D-64 (the ring gap at the reference load, and huge pages); its figures
follow those of #20 and settle contract section 16 (D-66).

## Hardware, and what it means

All phase 1 figures come from shared GitHub-hosted virtual machines, not
the target board: 4 vCPUs for the runs of issues #20 and #21, 2 for the
earlier run of issue #40. They run without a real-time kernel, with
neighbours on the same host.

| Runner | Processor | vCPUs | Clock source | Kernel |
| --- | --- | --- | --- | --- |
| `ubuntu-24.04` | AMD EPYC 9V74 | 4 | `tsc` | Ubuntu 24.04 image of the run |
| `ubuntu-24.04-arm` | Neoverse-N2 | 4 | `arch_sys_counter` | Ubuntu 24.04 image of the run |

"Separate cores" in conformance tests 3 and 26 means separate virtual
processors. The figures of issues #20 and #21 must be taken again on the target
board (issue #41) before phase 1 can be called done on it. Contract 1.0
does not wait for it (D-66); #41 stays open as the hardware acceptance
before the first release on the board.

Not measured in phase 1:

- the target board (#41);
- the jitter and overruns of a fast task on a real-time kernel (reference
  15.2).

The ring gap at the reference load and huge pages, listed here after
issue #20, were measured in issue #21 (below).

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

## Issue #20: the measurement run

Workflow `measurements`, run 43875571560, commit `0fd68a2` (main after PR
#94), `perf` build (GCC 13.3, `-O2 -g`, hardening flags), started by hand on
2026-11-16 with `seconds=600` and `latency_spread=true`, `runs=10`. Three
measurement jobs (x86-64 with `pause`, ARM64 with `isb` and with `yield`;
ARM64 ships `isb`, D-31) and 20 runs of conformance test 27 on fresh runners.
All 23 jobs passed.

### Conformance test 2: the reference load for 10 minutes

The acceptance line of the issue: 50,000 tags, 50,000 updates per second
sustained for 10 minutes with 4 readers. scada-sim writes 50,000 tags of 10
owners; 4 readers in another process read every tag with `read_many` of
500 and check every value; 600 s.

| | x86-64 | ARM64, `isb` | ARM64, `yield` |
| --- | --- | --- | --- |
| Updates per second | 50,000 | 50,000 | 50,000 |
| Reads | 41.8 billion | 51.5 billion | 51.2 billion |
| Torn reads | 0 | 0 | 0 |
| `SCADA_E_BUSY` | 0 | 3 | 19 |

### Conformance test 26: false trips, and the read retry limit

The reference load with 4 readers under SCHED_FIFO in a process marked
REALTIME (timeout 500 ms), reader i on the i-th processor, beside the
owners (timeout 3,000 ms). Each reader is a 10 ms task that reads 5,000
tags per cycle (2,000,000 reads per second in all); this load is fixed
(D-57). 600 s.

| | x86-64 | ARM64, `isb` | ARM64, `yield` |
| --- | --- | --- | --- |
| Reads | 1,200,240,000 | 1,200,220,000 | 1,200,220,000 |
| `SCADA_E_BUSY` | 0 | 0 | 149 |
| Rate per 1,000,000 reads | 0 | 0 | 0.124 |
| Bound of the test (99.9 % Poisson at 1 per million) | 1,309 | 1,309 | 1,309 |
| OWNER_DOWN reads, stale or late heartbeats | 0 | 0 | 0 |
| Torn, backwards or failed reads | 0 | 0 | 0 |
| Longest reader cycle | 1,190 us | 374 us | 773 us |

Criterion of contract section 16: fewer than 1 `SCADA_E_BUSY` in 1,000,000
reads. With the shipped code it is met on both runners in this run. The
second run (issue #21, below) gave 0.35 per million on ARM64 with `isb`:
two runs, 0 and 0.35 per million, are the evidence, not a bound from one
run. The limit of 64 attempts and the heartbeat defaults are confirmed (contract
0.22). In the pipeline, the 10 s run of the release build gates the same
criterion statistically (bound 35 for 20.24 million reads, D-57); in the dev
build, whose write path is not optimised, it is reported only: 0 to 31 in
about 20 million reads on a 4-processor container.

### Conformance test 27: latency of read and write

Under the reference load (scada-sim at 50,000 updates per second); each call
timed alone with CLOCK_MONOTONIC. Samples per run: 200,000 `read` and
`write`, 20,000 calls of 500 tags, 2,000 scans of 5,000 tags. Median of the
medians of 10 runs on fresh runners, their range, and the worst 99.9th
percentile of those 10 runs, in ns.

| Call | x86-64 median (range) | x86-64 worst p99.9 | ARM64 median (range) | ARM64 worst p99.9 |
| --- | --- | --- | --- | --- |
| One clock reading | 21 (20–30) | 80 | 32 (32) | 33 |
| `read` | 87 (80–100) | 461 | 104 (104) | 336 |
| `read_many`, 500 tags | 14,968 (10,696–17,012) | 48,510 | 18,272 (17,839–18,447) | 34,441 |
| `write` | 101 (80–120) | 351 | 136 (136) | 248 |
| `write_many`, 500 tags | 9,826 (8,153–14,287) | 39,223 | 19,900 (19,646–20,151) | 30,982 |
| `write_many`, 5,000 tags (the scan) | 97,810 (80,361–143,317) | 231,321 | 197,422 (193,340–198,812) | 296,449 |

The scan stays below the 0.5 ms of contract 16 at the median and at the
99.9th percentile in every run on the runners; on a busy 4-processor
container one local run gave 2.5 ms at the 99.9th percentile, so the worst
case is for the target board to decide. The x86-64 runners differ by up to
1.47 times from the median (two runs with a clock reading of 30 ns instead
of 20 ns, a different or busier host); ARM64 by 1.01 times. The medians of
the 10 runs are the baselines of the gate (`tests/perf/baseline/`), which
fails above 2.0 times them.

### Memory and database

Conformance test 27, during the reference load. Proportional set size (and
resident set size), kB:

| Process | x86-64 | ARM64 |
| --- | --- | --- |
| `scada-tagd` | 13,828 (20,464) | 13,677 (19,696) |
| `scada-sim`, 10 owner threads, 50,000 tags | 24,869 (31,488) | 24,721 (30,728) |
| A process that reads 50,000 and writes 5,000 tags | 10,028 (16,756) | 9,818 (15,960) |

The database of the reference load (all regions of the generation) is
28,825,112 bytes on both, below the 64 MB of contract 16. `scadactl`, the
remaining process of phase 1, was not measured; it runs only on demand.

### The other performance tests, measurement jobs

| | x86-64 | ARM64, `isb` | Bound |
| --- | --- | --- | --- |
| Wake-up, one consumer, p99 (2,500 samples) | 13 us | 17 us | 1 ms (D-32) |
| Wake-up, 8 observers, p99 (20,000 samples) | 40 us | 28 us | reported only (D-42) |
| Simulator, reference rate, worst lateness | 2 ms | 1 ms | 250 ms (D-41) |
| Simulator, one process at full speed | 33.3 million updates/s | 32.8 million updates/s | 50,000/s |
| Queue post, two contexts, per record | 282 ns | 377 ns | 10 us (D-49) |

Read retry window (`scada-tag-seqlock-bench`, as in issue #40): 64 attempts
on a record mid-write take 1,586 ns on x86-64 with `pause` and 665 ns on
ARM64 with `isb`; 50,000 scalar tags at 50,000 writes per second gave 0
`SCADA_E_BUSY` in 4.98 billion reads on x86-64 and 1 in 8.78 billion on
ARM64.

## Issue #21: the second measurement run

Workflow `measurements`, run 43905525505, commit `5bbfa71` (main after PR
#110, which added the tests of D-64), `perf` build, started by hand on
2026-11-16 with `seconds=600`. Three measurement jobs: x86-64 with `pause`,
ARM64 with `isb` and with `yield` (ARM64 ships `isb`, D-31). Same runners
as run 43875571560: shared virtual machines with 4 vCPUs (AMD EPYC 9V74,
Neoverse-N2).

### Ring gap at the reference load

`scada-tag-ring-gap-perf-test` (D-64): scada-sim writes 50,000 tags of 10
owner threads at 50,000 updates per second with the default ring capacity
of 65,536 entries; one consumer thread per ring takes every change. Any gap
fails the test. 600 s.

| | x86-64 | ARM64, `isb` | ARM64, `yield` |
| --- | --- | --- | --- |
| Changes received | 30,000,000 | 30,000,000 | 30,000,000 |
| Gaps | 0 | 0 | 0 |

The 10 s form, in the same jobs and in the pipeline's perf gate, received
500,000 changes with 0 gaps on each runner. This settles the gap half of
the ring capacity in contract 16; the size half is 28.8 MB (issue #20).
The default is settled for the reference load of 10 owners. The capacity
is per owner in the tag set, and phase 2 sizes each owner's ring.

### Huge pages

`scada-tag-huge-pages-root-test` (D-64): one owner of 50,000 tags (3.2 MB),
written at 50,000 updates per second, read 1,000,000 times with `read` and
20,000 times with `read_many` of 500, with the database on a tmpfs mounted
`huge=never` and on one mounted `huge=always`. One run of each, about
0.6 s. Times in ns.

| Runner | Mode | `read` median | `read` p99.9 | `read_many` 500 median | `read_many` 500 p99.9 | kB mapped huge |
| --- | --- | --- | --- | --- | --- | --- |
| x86-64 | never | 80 | 261 | 14,521 | 31,226 | 0 |
| x86-64 | always | 80 | 270 | 14,521 | 29,704 | 2,048 |
| ARM64, `isb` | never | 120 | 312 | 18,479 | 28,174 | 0 |
| ARM64, `isb` | always | 104 | 272 | 18,503 | 25,655 | 2,048 |
| ARM64, `yield` | never | 144 | 416 | 18,342 | 32,878 | 0 |
| ARM64, `yield` | always | 104 | 384 | 18,343 | 27,302 | 2,048 |

Huge pages are not used (D-66), a judgement on single runs of about
0.6 s, one per mode and runner. There is no gain on x86-64: the read
medians are equal at 80 ns, and the read 99.9th percentile is 261 ns
without and 270 ns with huge pages. On ARM64 the read median was up to
about 40 ns lower (`isb` 120 to 104 ns, `yield` 144 to 104 ns), within the
noise of single 0.6 s runs, and not needed for any target; D-64 asked for
a lower 99.9th percentile on both architectures. An owner region at the
reference load (320 KB) is smaller than one 2 MB page. To be measured
again on the target board (#41) if a target there needs it.

### Conformance test 2, second run

As in issue #20, 600 s.

| | x86-64 | ARM64, `isb` | ARM64, `yield` |
| --- | --- | --- | --- |
| Updates per second | 50,000 | 49,999 | 50,000 |
| Reads | 41.8 billion | 51.6 billion | 51.5 billion |
| Torn reads | 0 | 0 | 0 |
| `SCADA_E_BUSY` | 0 | 1 | 13 |

### Conformance test 26, second run

As in issue #20, 600 s. The first run (43875571560) beside the second
(43905525505):

| | x86-64 | ARM64, `isb` | ARM64, `yield` |
| --- | --- | --- | --- |
| Reads, second run | 1,200,240,000 | 1,200,220,000 | 1,200,220,000 |
| `SCADA_E_BUSY`, first run | 0 | 0 | 149 |
| `SCADA_E_BUSY`, second run | 1 | 424 | 169 |
| Rate per 1,000,000 reads, first run | 0 | 0 | 0.124 |
| Rate per 1,000,000 reads, second run | 0.0008 | 0.35 | 0.14 |
| Bound of the test | 1,309 | 1,309 | 1,309 |
| Torn, backwards, down, overruns, second run | 0 | 0 | 0 |
| Longest reader cycle, second run | 1,397 us | 491 us | 351 us |

Every job passed. The rate varies between runs on shared virtual machines:
ARM64 with the shipped `isb` gave 0 in 1.20 billion reads in the first run
and 424 in the second. Both are below the criterion of 1 per million; the
margin in the second run was about 3 times. The limit of 64 attempts stays
(D-66). The target board (#41) is to measure it again. The test measures
real-time readers on processors they share with non-real-time owners. The
limit of 64 is a constant of the library, not of the layout: a worse figure
on the board changes core placement (#116), not the layout.

### Conformance test 27, medians against the baselines

One run of each job, in ns, as "run (baseline)". The baselines are those of
issue #20 (`tests/perf/baseline/`); `yield` is not shipped and is given
without one.

| Call | x86-64 | ARM64, `isb` | ARM64, `yield` |
| --- | --- | --- | --- |
| `read` | 80 (87) | 104 (104) | 104 |
| `read_many`, 500 tags | 14,501 (14,968) | 18,063 (18,272) | 18,439 |
| `write` | 100 (101) | 136 (136) | 136 |
| `write_many`, 500 tags | 12,419 (9,826) | 20,103 (19,900) | 19,727 |
| `write_many`, 5,000 tags (the scan) | 123,912 (97,810) | 198,788 (197,422) | 194,076 |

On x86-64 `write_many` reached 1.26 times its baseline for 500 tags and
1.27 times for 5,000 tags, within the 2.0 times tolerance of the gate (the
slowest of the 10 runs of issue #20 was 1.47 times). The scan stays below the 0.5 ms of contract 16.

## Phase 1 issues at the close

Contract changes merged after the measurements of issue #20, all before 1.0:

| Issue | Pull request | Contract | Change |
| --- | --- | --- | --- |
| #92 | #95 | 0.23 | A consumer that sees a queue sees its producer (D-61) |
| #96 | #111 | 0.24 | Minimum interval between two writes of one owner's retain file. A RETAIN_NOW change is on disk within 1 s while a write, queued writes included, takes at most 200 ms; a tag set with a RETAIN_NOW tag has `heartbeat_check_ms` of at most 400 ms; the warning `retain_now change reached the disk late` (D-62) |
| #21 | #113 | 0.25 | Wording promised for 1.0; no behaviour changed (D-63) |
| #112 | #114 | 0.26 | `scada_tag_write_string` returns `SCADA_E_NOT_OWNER` before it checks the text (D-65) |
| #21 | #110 and this report's pull request | 1.0 | The tests of D-64; contract section 16 settled (D-66) |

Carried forward, open:

- #41: the phase 1 figures on the target board, the hardware acceptance
  before the first release on the board.
- #6 (P0-05): the acceptance of phase 0 on a test machine (restart on a hung
  loop, read-only root), with #41.
