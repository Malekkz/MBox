# C++ coding standard

This standard applies to all C++ in this repository. Its main purpose is to
prevent memory errors, undefined behaviour and data races in a system that
controls equipment and parses data from the network.

C++ cannot be made memory-safe by rules alone. The rules reduce how many
errors are written; the tools in section 13 catch most of the rest; the
separate processes of the architecture limit what the remainder can damage.

## 1. Baseline and precedence

- The baseline is the C++ Core Guidelines. Where this document is silent, they
  apply.
- This document is stricter than the baseline in places. Where they differ,
  this document wins.
- `CLAUDE.md` holds the short version of these rules. If the two disagree, stop
  and ask.
- Language level is C++20, with the standard library only, plus dependencies
  listed in the licence report.

## 2. Compiler settings

All builds, on both architectures:

```
-Wall -Wextra -Wpedantic -Werror
-Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wcast-align
-Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
-Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
-D_GLIBCXX_ASSERTIONS
```

Release builds add:

```
-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection
-fPIE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
-fcf-protection=full          (x86-64)
-mbranch-protection=standard  (ARM64)
```

- `_GLIBCXX_ASSERTIONS` turns out-of-range access on standard containers,
  `std::span` and `std::string_view` into an immediate abort instead of silent
  corruption. It stays on in release builds.
- If the toolchain does not support a flag, record the substitute in
  `docs/decisions.md`. Never drop a flag silently.
- A warning is never disabled for a whole target. See section 14 for single
  suppressions.

## 3. Ownership and lifetime

- Every resource has exactly one owner, and the owner is an object whose
  destructor releases it (RAII). This covers memory, file descriptors, mapped
  regions, sockets, locks and threads.
- No `new`, `delete`, `malloc` or `free` in project code. Use values first,
  then `std::unique_ptr` with `std::make_unique`. `std::shared_ptr` needs a
  stated reason in a comment.
- A raw pointer or reference never owns. It means "borrowed, valid for this
  call". A function that keeps a pointer or reference beyond its own call must
  say so in its contract comment.
- Never return or store a reference, pointer, `std::string_view` or `std::span`
  to a local variable, a temporary, or a container element that can be
  invalidated.
- `std::string_view` and `std::span` are for parameters. Do not store them in
  objects unless the object's contract states what they point into and why it
  outlives the object.
- Do not hold iterators, pointers or references into a container across any
  call that can grow or shrink it.
- Do not use an object after `std::move`, except to assign to or destroy it.
- Lambdas that outlive the current scope capture by value. A capture by
  reference in a callback, thread or queued task is an error.
- File descriptors and mapped regions are wrapped in owning types (`Fd`,
  `MappedRegion`) from `libscada-common`. Raw `close` and `munmap` appear only
  inside those types.

## 4. Bounds and buffers

- No C arrays, except inside the fixed-layout structures of a contract. Use
  `std::array` for fixed sizes and `std::vector` otherwise.
- No pointer arithmetic and no indexing through a raw pointer. Pass
  `std::span<T>` (pointer and length together).
- Use `.at()` or a checked accessor wherever an index comes from outside the
  function. Unchecked `[]` is allowed only where the index is provably in range
  from the surrounding code, such as a loop over the container's own size.
- `memcpy`, `memmove` and `memset` appear only in a small set of reviewed
  helper functions that take spans and check sizes. Use `std::copy`,
  `std::fill` and `std::bit_cast` elsewhere.
- Fixed-size name fields in contract structures are filled and read only
  through helpers that check length and handle the missing terminator.
- Buffers for network frames and files have a stated maximum size. A length
  field read from input is checked against the bytes actually available and
  against that maximum before any use.

## 5. Integers and conversions

- No implicit narrowing or sign conversion; the compiler flags enforce this.
  Convert explicitly with a checked helper (`narrow<T>(x)` returns an error on
  loss) when the value comes from outside, or `static_cast` when the range is
  proven.
- Sizes and counts are `std::size_t`. Protocol and layout fields use the
  fixed-width types named in their specification.
- Arithmetic on values from outside (lengths, offsets, counts, scaling
  factors) is checked for overflow before the result is used as a size, index
  or allocation.
- Signed overflow is undefined behaviour. Do not rely on wrapping; use
  unsigned types or checked helpers where wrapping is intended, and say so.
- No C-style casts. `reinterpret_cast` appears only in the reviewed helpers
  that map contract structures onto memory. `const_cast` is not used.
- Enumerations are `enum class`. A value read from outside is validated before
  it is converted to an enumeration.

## 6. Initialisation and types

- Every variable is initialised where it is declared. Every class member has a
  default member initialiser or is set in every constructor.
- Single-argument constructors are `explicit`.
- A class either manages a resource and defines or deletes all five special
  member functions, or manages none and declares none (rule of five, rule of
  zero).
- Base classes with virtual functions have a virtual destructor. Overrides are
  marked `override`.
- Prefer `std::optional`, `std::variant` and `Result<T>` to sentinel values and
  out-parameters.
- No `union` outside contract structures. No type punning through unions; use
  `std::bit_cast` or the reviewed helpers.
- `const` by default: variables that do not change, member functions that do
  not modify, parameters passed by `const&` or by value.

## 7. Error handling

- Functions that can fail return `Status` or `Result<T>`, marked
  `[[nodiscard]]`. A result is never ignored; if ignoring is correct, say why
  in a comment.
- No exceptions cross a library boundary, and real-time code contains none.
  Code that calls a throwing library function catches at the call site and
  converts to `Status`.
- Destructors, move operations and `swap` do not fail.
- `assert`-style checks state programmer assumptions and stay enabled in
  release builds for memory-safety conditions (bounds, null, state). They never
  replace validation of outside input, which must return an error.
- On a broken invariant that makes continuing unsafe, the process logs and
  aborts. The service supervisor restarts it; that is safer than running on
  corrupted state.
- `std::_Exit` is called only in `main`, after a bounded stop failed (a
  thread that cannot be joined within its bound, such as a write in a hung
  `fsync`; decision D-59): log, flush the log, then `_Exit` with a distinct
  exit status. Nowhere else, and never in place of error handling.

## 8. Concurrency

- Data shared between threads is either immutable, owned by one thread and
  passed by message, `std::atomic`, or protected by a mutex that the type
  itself holds. Nothing else.
- Locks are taken with `std::scoped_lock` or `std::unique_lock`, never by
  manual `lock`/`unlock`. Lock order is documented wherever two locks can be
  held together.
- No lock is held across a call into code that can block, call back, or take
  another lock that is not in the documented order.
- Threads are `std::jthread` or a wrapper that joins on destruction. No
  detached threads.
- Atomics state their memory order explicitly and carry a comment when the
  order is weaker than sequentially consistent.
- Condition waits always use a predicate. Tests wait on conditions with
  timeouts, never on sleeps.
- No signal handlers beyond setting a flag or writing to a self-pipe.

## 9. Shared memory

The tag database is memory written by other processes. Treat everything read
from it as input, even though the writer is our own code: a bug or a crash in
one process must not corrupt another.

- Structures placed in shared memory are standard-layout and trivially
  copyable, contain no pointers, references, virtual functions or standard
  containers, and have their size and offsets fixed by `static_assert`.
- Positions in shared memory are offsets or indexes, never addresses.
- Every offset, length, index, count, handle and enumeration value read from a
  region is validated against the region's actual mapped size and the limits in
  the contract before it is used.
- Region files are checked for magic, layout version, generation, size and
  owning user before use, as the tag database contract requires.
- Raw access to mapped memory exists only inside `libscada-tag`. Everything
  else uses its interface.
- The sequence-lock read is the one place where a non-atomic field is read
  while another process may write it. It is implemented once, in
  `libscada-tag`: copy the bytes into a local object, re-check the sequence,
  and use the copy only if the check passes. No other code reads shared fields
  directly, and no decision is ever made on an unvalidated copy.
- A reader never waits without a bound on a writer in another process. Loops
  over shared state have a retry limit.
- Regions that a process does not own are mapped read-only. Code must not
  depend on being able to write to them.

## 10. Parsing untrusted input

This applies to protocol frames, API requests, configuration files, imported
archives and SVG, and the node link.

- Parse into typed values first, then act. No action is taken on partially
  parsed input.
- Parsers take a `std::span<const std::byte>` or a `std::string_view` and read
  through a cursor type that checks every read against the remaining length.
- Every parser has a stated maximum for message size, nesting depth, element
  count and string length, and rejects input beyond it.
- Lengths and counts in the input never drive an allocation or a loop bound
  before they are checked against those maximums and the bytes available.
- Text is validated as UTF-8 where it must be text. Names are checked against
  the grammar in the contract before they are used in a path, a query or a log
  line.
- No input-controlled format strings, shell commands or file paths. Paths are
  built from validated names under a fixed directory, and resolved paths are
  checked to stay inside it.
- Every parser ships with a fuzz target in the same pull request (section 13).

## 11. Real-time code

The rules in `CLAUDE.md` apply: no allocation, no blocking, no system calls,
no logging inside `SCADA_RT` functions. In addition:

- A short retry loop on shared state (the sequence-lock read) pauses with
  `scada::cpu_relax()` from `libscada-common`. That function is the only
  place outside the I/O HAL that names a processor instruction; C++20 has no
  portable spelling for it. No other code uses `asm`.
- All memory a real-time task needs is allocated and touched before the task
  starts. Containers used inside it have their capacity reserved and never
  grow.
- Real-time code uses no `std::function`, no exceptions, no `std::string`
  construction, and no stream I/O.
- Data passed from a real-time thread to another thread goes through a
  pre-allocated lock-free queue.

## 12. Banned

| Banned | Use instead |
| --- | --- |
| `new`, `delete`, `malloc`, `calloc`, `realloc`, `free` | Values, `std::make_unique`, containers |
| `strcpy`, `strcat`, `sprintf`, `vsprintf`, `gets`, `strtok` | `std::string`, `std::format_to_n`, checked helpers |
| `strncpy`, `strlen` on outside data | Length-checked helpers on spans |
| `atoi`, `atof`, `strtol` without full error checks | `std::from_chars` |
| `scanf` family | A parser with a checked cursor |
| `alloca`, variable-length arrays | `std::array` or a pre-sized container |
| C-style casts, `const_cast` | Named casts, checked helpers |
| `reinterpret_cast`, raw `memcpy` outside reviewed helpers | `std::bit_cast`, span-based helpers |
| `std::rand`, `rand` | `<random>`; the operating system's source for security use |
| `system`, `popen` | Direct system calls with fixed arguments, in the privileged helper only |
| `using namespace` in headers | Qualified names |
| Detached threads, manual `lock`/`unlock` | `std::jthread`, scoped locks |
| `goto`, `setjmp`, `longjmp` | Structured control flow |

## 13. Tools and gates

Every row runs on every pull request unless stated. A finding fails the build.

| Tool | Catches | Where |
| --- | --- | --- |
| Compiler warnings as errors (section 2) | Narrowing, shadowing, missing cases, format errors | All builds |
| clang-format | Layout differences | `check` target |
| clang-tidy | Ownership, lifetime, bounds and bug patterns | `check` target |
| Banned-function check | Section 12 | `check` target |
| `SCADA_RT` check | Allocation, locks and logging in real-time code | `check` target |
| AddressSanitizer with leak detection, plus UndefinedBehaviorSanitizer | Overflows, use after free, leaks, undefined behaviour | All unit and integration tests, x86-64 |
| ThreadSanitizer | Data races, lock-order problems | All unit and integration tests, x86-64 |
| Library assertions (`_GLIBCXX_ASSERTIONS`) | Out-of-range container access | All builds |
| Fuzz targets (libFuzzer with sanitizers) | Parser crashes and hangs | Short fixed run per pull request; long runs nightly once the pipeline supports it |
| Memory growth test | Slow leaks | Soak tests at the end of each phase |

clang-tidy check groups, all enabled: `bugprone-*`, `cert-*`,
`clang-analyzer-*`, `concurrency-*`, `cppcoreguidelines-*`, `misc-*`,
`modernize-*`, `performance-*`, `readability-*`. Findings in `bugprone-*`,
`cert-*`, `clang-analyzer-*`, `concurrency-*` and `cppcoreguidelines-*` are
errors. Individual checks that prove unworkable are disabled in
`.clang-tidy` with a one-line reason and an entry in `docs/decisions.md`.

The sequence-lock read in `libscada-tag` is reported by ThreadSanitizer by
design. It is the only permitted suppression of a race report, it is annotated
at that one function, and the suppression file contains nothing else.

## 14. Deviations

- A single finding may be suppressed only on the line it occurs, naming the
  check and giving a reason: `// NOLINT(check-name): reason`.
- A suppression without a named check and a reason fails review.
- The banned-function and `SCADA_RT` checks are named `scada-banned` and
  `scada-rt`. They ignore a `NOLINT` that does not name them.
- Every suppression added by a pull request is listed in its description.
- Suppressions in code covered by "Safety and security rules" in `CLAUDE.md`
  need review by a person.

## 15. Review checklist for memory safety

A reviewer, human or agent, answers these for every pull request that adds or
changes C++:

1. Who owns each new resource, and where is it released?
2. Can any pointer, reference, view or span outlive what it refers to?
3. Is every index, length and offset from outside the function checked before
   use?
4. Can any arithmetic on outside values overflow before it is checked?
5. What is shared between threads or processes, and what protects it?
6. Is every failure path handled, with no ignored result?
7. Does new parsing code have a size limit and a fuzz target?
8. Do the tests include the invalid, oversized and truncated cases?
9. Are there new suppressions, and is each one justified?
