#ifndef SCADA_TAG_THREAD_MARK_H
#define SCADA_TAG_THREAD_MARK_H

// What tells one thread from another without a system call (contract,
// section 13.2). Private to libscada-tag.

#include "scada/common/realtime.h"

namespace scada::tag {

// The address of an object every thread has its own of. Two threads that
// run at the same time get different addresses; a thread that has ended
// may leave its address to a new one. The library is linked statically
// (decision D-5), so the access needs no call of the dynamic linker.
SCADA_RT [[nodiscard]] inline const void* thread_mark() noexcept {
    thread_local const char MARK = 0;
    return &MARK;
}

}  // namespace scada::tag

#endif  // SCADA_TAG_THREAD_MARK_H
