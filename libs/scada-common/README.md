# libscada-common

Code shared by every component (reference 3.3): error values, structured
logging (reference 14.6), clocks, an owning file descriptor and the real-time
annotation.

## What it does

- `Status` and `Result<T>`: errors returned as values. A `Status` is a code
  plus a fixed message; it never allocates, so real-time code may return it.
- `Logger`: structured log records (severity, message, up to 8 key/value
  fields) to the systemd journal, with the standard error stream as fallback.
- `realtime_ns()` and `monotonic_ns()`: `CLOCK_REALTIME` and `CLOCK_MONOTONIC`
  in nanoseconds.
- `Fd`: owns a file descriptor and closes it.
- `panic()`: ends the process after a broken invariant.
- `SCADA_RT`: the annotation that marks a function as real-time (`CLAUDE.md`,
  "Real-time code"). The library does not enforce the real-time rules itself;
  the `check` target does, see `tools/check/README.md`.

It does not provide `MappedRegion` or `narrow<T>` (coding standard, sections 3
and 5); those are added by the first issue that needs them.

No exception leaves the library. Functions that can fail return `Status` or
`Result<T>`.

## Logging behaviour

- `Logger::log` copies the record into a queue of fixed size and returns. A
  worker thread hands records to the sink, so a caller never waits for the
  journal or a pipe.
- If the queue is full, the new record is dropped and counted. When the sink
  accepts records again, one `WARNING` record reports the count in the field
  `DROPPED`.
- Limits per record: message 384 bytes, key 32 bytes, value 96 bytes, 8
  fields. Longer text is cut at a UTF-8 character boundary. Default queue
  capacity: 256 records, about 1.5 KiB each.
- Field keys follow the journal's rule (`A-Z`, `0-9`, `_`, starting with a
  letter). A field with an invalid or reserved key is omitted.
- The journal sink sends one non-blocking datagram per record in the journal's
  native protocol, with `MESSAGE`, `PRIORITY`, `SYSLOG_IDENTIFIER`,
  `SCADA_TIME_NS` (creation time, `CLOCK_REALTIME`) and the record's fields.
- The stream sink writes one line of at most 4096 bytes, with control
  characters escaped, and gives up after 100 ms if the stream is not writable.
- `make_default_sink()` uses the journal when the service manager connected
  the standard error stream to it (`JOURNAL_STREAM`), otherwise the standard
  error stream.
- Logging is not allowed in real-time code.

## Interfaces

- Public headers in `include/scada/common/`, namespace `scada`: `status.h`,
  `result.h`, `log.h`, `clock.h`, `fd.h`, `panic.h`, `static_string.h`,
  `realtime.h`.
- Uses the journal socket `/run/systemd/journal/socket`.
- No contract; decisions D-9, D-14 and D-15 in `docs/decisions.md`.

## How to use it

Link the CMake target `scada::common`:

```cmake
scada_add_library(scada-example SOURCES src/example.cpp DEPENDS scada::common)
```

```cpp
#include "scada/common/log.h"

scada::Result<int> parse_port(std::string_view text);  // returns a value or a Status

auto sink = scada::make_default_sink();
if (!sink.ok()) { return sink.status(); }
auto logger = scada::Logger::create(std::move(sink).value(), {"scada-example"});
if (!logger.ok()) { return logger.status(); }

logger.value().log(scada::LogLevel::WARNING, "device not answering",
                   {{"DEVICE", name}, {"ATTEMPT", std::to_string(attempt)}});
```

## How to test it

```
ctest --preset dev -R 'scada-common|journal_stream'
```

- `scada-common-unit-test`: the unit tests. They need no fixture; the sink
  tests create a pipe and a datagram socket in a temporary directory in place
  of the journal.
- `journal_stream_fuzz`: fuzz target for the parser of `JOURNAL_STREAM`, in
  `fuzz/`. `ctest --preset fuzz` fuzzes it for 60 s; every other build replays
  its seed corpus as `journal_stream_fuzz-corpus`.
