// A read attempt of the shape of section 6.2 whose unlikely branch GCC 13
// moves to a part of its own, `[clone .cold]`, when asked to partition
// (issue #63). GCC 13.3 does not split the protocol functions themselves, so
// the self-test checks the handling of such a part on this code instead of
// on synthetic text. Compiled for the fixture gcc13-aarch64-O2-cold.txt; see
// tests/model/README.md. Not part of any build.

#include <atomic>
#include <cstdint>

namespace scada::model_fixture {

struct Record {
    std::atomic<std::uint32_t> seq;
    std::atomic<std::uint64_t> a;
    std::atomic<std::uint64_t> b;
};

// Cold and kept out of line (noipa: an empty body would otherwise drop the
// call): its call is the unlikely branch GCC splits off.
[[gnu::cold, gnu::noipa]] void note_busy(int attempt) {
    static_cast<void>(attempt);
}

bool read_pair(Record& record, std::uint64_t& a, std::uint64_t& b) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t seq = record.seq.load(std::memory_order_acquire);
        if ((seq & 1U) != 0U) [[unlikely]] {
            note_busy(attempt);
            continue;
        }
        a = record.a.load(std::memory_order_relaxed);
        b = record.b.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (record.seq.load(std::memory_order_relaxed) == seq) {
            return true;
        }
    }
    return false;
}

}  // namespace scada::model_fixture
