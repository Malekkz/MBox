#ifndef SCADA_COMMON_REALTIME_H
#define SCADA_COMMON_REALTIME_H

// SCADA_RT marks a function as real-time: it runs inside a control task, the
// I/O scan or one of the tag library's real-time functions, and must not
// allocate, block, lock, log or make system calls (CLAUDE.md, Real-time code;
// docs/coding-standard.md, section 11).
//
// Put it first in the declaration that other code sees; the definition
// inherits it:
//
//     SCADA_RT Status read(TagHandle tag, Value& value) noexcept;
//
// The `check` target finds the marked functions and reports calls they must
// not make (tools/check/README.md). The macro changes nothing in the compiled
// code. Compilers without the attribute see an empty macro, so only a Clang
// based tool can run the check.
//
// Project code must not use the `annotate` attribute for anything else: the
// check treats every function that carries it as real-time.
#if defined(__has_cpp_attribute) && __has_cpp_attribute(clang::annotate)
#define SCADA_RT [[clang::annotate("scada_rt")]]
#else
#define SCADA_RT
#endif

#endif  // SCADA_COMMON_REALTIME_H
