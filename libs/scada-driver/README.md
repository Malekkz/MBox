# libscada-driver

The driver framework that every protocol driver and `scada-io` are built on
(issue #104, P2-07). The behaviour is that of the driver interface contract,
`docs/contracts/driver-interface.md` (version 0.5); the configuration keys
are those of the configuration schemas contract.

The library is built in slices. This is the first:

| Part | Header | Contract |
| --- | --- | --- |
| Raw values, as a protocol's decode delivers them | `scada/driver/raw_value.h` | 5.1, 5.2 |
| Compiled rules of a point's common keys | `scada/driver/point_rules.h` | 5.1 to 5.8; CS 4.7, 4.9, 5.3 to 5.5 |
| Processing after decode, staleness | `scada/driver/sample_processing.h` | 5.1 to 5.8, 7.5 |
| Device local time to UTC | `scada/driver/zone_table.h` | 5.8; CS 4.6 (decision D-130) |
| Engineering value to raw value for a write | `scada/driver/write_conversion.h` | 8.4; CS 7.2 |

Not yet here: the protocol interface (2.2), the object model, scheduling,
transports, the driver process with its threads, commands, the generation
change and reload. `docs/decisions.md` D-130 to D-133 record the choices
the contract left open.

## What it does

- `process_sample` takes one raw sample of a point (an outcome, the raw
  pattern and number), the point's rules and run-time state, and the
  samples of its companions in the same poll. It applies, in order:
  invalid markers on the pattern, non-finite values, the exponent, the
  scale, counters, double points, enumerations and bits, the quality rules
  and health rules, and the time of the sample. It returns the writes of
  the point's tag and of its bit tags. It allocates nothing, takes no lock
  and reads no clock (`SCADA_RT`).
- `check_stale` adds STALE once to a point that no read confirmed within
  its `max_age`, and turns a `keep_last` value BAD after `max_age`.
- `to_raw_value` converts a write's engineering value to the raw value, on
  the exact integer path or in binary64, and refuses what does not convert
  exactly as the contract states.
- The library is compiled with floating-point contraction off
  (`-ffp-contract=off`, decision D-98), so scaled values have the same bits
  on x86-64 and ARM64.

## Testing

```
cmake --build --preset dev --target scada-driver-unit-test
ctest --preset unit -R scada-driver
```

The golden vectors (`tests/golden_samples_test.cpp`,
`tests/golden_write_test.cpp`) compare value bits, so a difference between
the architectures fails them. The tests of local device times need the
system's time zone database (`Europe/Berlin`).
