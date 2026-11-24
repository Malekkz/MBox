// Calls that a SCADA_RT function must not make.

#include "scada/common/realtime.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace fixture {

std::mutex guard;
std::shared_mutex shared_guard;

SCADA_RT int allocate_with_make_unique(int value) {
    const auto copy = std::make_unique<int>(value);  // expect: scada-rt
    return *copy;
}

SCADA_RT int allocate_with_new(int value) {
    const int* copy = new int(value);  // expect: scada-rt
    const int result = *copy;
    delete copy;  // expect: scada-rt
    return result;
}

SCADA_RT void* allocate_with_malloc(std::size_t size) {
    return std::malloc(size);  // expect: scada-rt
}

SCADA_RT void log_with_printf(int value) {
    std::printf("%d\n", value);  // expect: scada-rt
}

SCADA_RT int lock_scoped(int value) {
    const std::scoped_lock lock(guard);  // expect: scada-rt
    return value;
}

SCADA_RT int lock_shared(int value) {
    const std::shared_lock lock(shared_guard);  // expect: scada-rt
    return value;
}

SCADA_RT int lock_manually(int value) {
    guard.lock();  // expect: scada-rt
    guard.unlock();
    return value;
}

// The annotation is on the declaration only; the definition inherits it.
SCADA_RT int declared_real_time(int value);

int declared_real_time(int value) {
    return *std::make_unique<int>(value);  // expect: scada-rt
}

class Scanner {
public:
    SCADA_RT int scan(int value) const;
};

int Scanner::scan(int value) const {
    const std::lock_guard<std::mutex> lock(guard);  // expect: scada-rt
    return value;
}

// A lambda inside a real-time function is part of it.
SCADA_RT int allocate_in_lambda(int value) {
    const auto make = [value] {
        return std::make_shared<int>(value);  // expect: scada-rt
    };
    return *make();
}

// An override does not inherit the annotation; it must repeat it.
class Protocol {
public:
    virtual ~Protocol() = default;
    SCADA_RT virtual int encode(int value) const = 0;
    virtual int configure(int value) const = 0;
};

class Forgets : public Protocol {
public:
    int encode(int value) const override;  // expect: scada-rt
    int configure(int value) const override;
};

class Repeats : public Protocol {
public:
    SCADA_RT int encode(int value) const override;
    int configure(int value) const override;
};

// Not real-time: none of these lines is reported by the SCADA_RT check.
int not_real_time(int value) {
    const std::scoped_lock lock(guard);
    std::printf("%d\n", value);
    return *std::make_unique<int>(value);
}

}  // namespace fixture
