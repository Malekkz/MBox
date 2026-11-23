# libscada-driver

The driver framework that every protocol driver and `scada-io` are built on
(issue #104, P2-07). The behaviour is that of the driver interface contract,
`docs/contracts/driver-interface.md` (version 0.5); the configuration keys
are those of the configuration schemas contract.

The library is built in slices. This is the first:

| Part | Header | Contract |
| --- | --- | --- |
| Raw values, as a protocol's decode delivers them | `scada/driver/raw_value.h` | 5.1, 5.2 |
| Compiled rules of a point's common keys, and their check | `scada/driver/point_rules.h` | 5.1 to 5.8; CS 4.7, 4.9, 5.3 to 5.5 |
| Processing after decode, staleness | `scada/driver/sample_processing.h` | 5.1 to 5.8, 7.5 |
| Device local time to UTC, from the unit's zone files | `scada/driver/zone_table.h` | 5.8; CS 4.6 (decision D-130) |
| Engineering value to raw value for a write; raw patterns | `scada/driver/write_conversion.h` | 8.4; CS 7.2 |

Not yet here: the protocol interface (2.2), the object model, scheduling,
transports, the driver process with its threads, commands, the generation
change and reload. `docs/decisions.md` D-130 to D-133 record the choices
the contracts left open.

## What it does

- `process_sample` takes one raw sample of a point (an outcome, the raw
  pattern and number), the point's rules and run-time state, and the
  samples of its companions in the same poll with the quality their own
  processing gave them. It applies, in order: invalid markers on the
  pattern, non-finite values, the exponent, the scale, counters, double
  points, enumerations and bits, the quality and health rules (for every
  outcome), and the time of the sample. It fills a result the caller keeps
  (the point's tag and its bit tags). It allocates nothing, takes no lock
  and reads no clock (`SCADA_RT`).
- `check_stale` adds STALE once to a point that no read confirmed within
  its `max_age`, and turns a held value (`keep_last`, DEVICE_INVALID) BAD
  after `max_age`.
- `to_raw_value` converts a write's engineering value to the raw value, on
  the exact integer path or in binary64, and returns the engineering value
  the raw value reads back as; it refuses what does not convert exactly as
  the contract states. `raw_from_pattern` gives the raw value of a
  configured raw pattern (a safe state or a release value).
- `validate_point_rules` states what the compile step must refuse; rules
  that reach the processing anyway give BAD, never undefined behaviour.
- The library is compiled with floating-point contraction off
  (`-ffp-contract=off`, decision D-98), so scaled values have the same bits
  on x86-64 and ARM64.

## Known behaviour

- **Counter wraps.** A decrease from at least `limit`/2 to below it is a
  wrap (contract 5.4): it is written GOOD with no flag, whatever
  `on_decrease` says. A 32-bit counter that steps back from 3,000,000,000 to
  2,000,000,000 is therefore read as a wrap. A 64-bit counter never comes
  near half its limit.
- **Setpoints are rounded to the raw step.** On the binary64 path with an
  integer raw type, 12.34 on `divide_by: 10` is written as raw 123, which
  reads 12.3; `to_raw_value` returns 12.3 as the value written.
- **SUBSTITUTED** is not written until tag database contract 1.1 (#89): a
  configured substitute is UNCERTAIN, and incoming flags are masked.

## Zone files

Device local times are converted with the unit's compiled zone files
(TZif): the directory named by the environment variable `TZDIR`, else the
one the CMake option `SCADA_ZONEINFO_DIR` sets (default
`/usr/share/zoneinfo`). A target image must carry them there. They are read
when a configuration is compiled.

## Testing

```
cmake --build --preset dev --target scada-driver-unit-test scada-driver-zones-test
ctest --preset unit -R 'scada-driver|sample_processing|tzif'
ctest --preset integration -R scada-driver-zones
```

- The golden vectors are in `tests/golden/driver-vectors.txt`, a data file
  that the protocols and their simulators share (its header gives the
  format); `tests/golden_vectors_test.cpp` runs them. They compare value
  bits, so a difference between the architectures fails them.
- `scada-driver-zones-test` compares every zone of the build machine's
  database with the C library's reading of the same files, by a full search
  around every change from 1970 to 2100 (about a million local times, some
  15 s).
- The unit tests of local times need the zones `Europe/Berlin`,
  `America/Vancouver`, `America/New_York`, `Australia/Sydney`,
  `Australia/Lord_Howe`, `Europe/Dublin` and `Etc/GMT-3`; a machine without
  them fails the tests, it does not skip them.
- Fuzz targets: `sample_processing_fuzz` (samples, rule fields, a local
  zone, the write conversion) and `tzif_fuzz` (the zone file reader and the
  TZ string parser).
