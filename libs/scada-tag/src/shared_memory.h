#ifndef SCADA_TAG_SHARED_MEMORY_H
#define SCADA_TAG_SHARED_MEMORY_H

// The reviewed helpers that place contract structures on mapped memory and
// access the fields that change while other processes read them (coding
// standard, sections 5 and 9). No other code in this library casts mapped
// bytes, and no code outside this library reads them at all.

#include "layout.h"

#include "scada/common/panic.h"
#include "scada/common/realtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace scada::tag {

// True if an object of `size` bytes fits in `region_size` bytes at `offset`.
SCADA_RT [[nodiscard]] constexpr bool fits(std::size_t region_size,
                                           std::size_t offset,
                                           std::size_t size) noexcept {
    return offset <= region_size && size <= region_size - offset;
}

// Returns the structure of type `Shared` that lies at `offset` in the mapped
// `region`. The reference is valid as long as the mapping is.
//
// The caller has validated `offset` against the mapped size before: an
// object that does not fit, or an address that is not aligned for `Shared`,
// is a defect of this library and ends the process. `region` starts at a
// page boundary, so alignment depends on `offset` alone.
//
// Fields marked `A` in layout.h are then accessed only through
// `load_shared` and `store_shared`.
template <typename Shared>
    requires IS_SHARED_STRUCT<Shared> || std::is_integral_v<Shared>
SCADA_RT [[nodiscard]] Shared& shared_at(std::span<std::byte> region,
                                         std::size_t offset) noexcept {
    if (!fits(region.size(), offset, sizeof(Shared)) ||
        offset % alignof(Shared) != 0) {
        panic("shared memory: object outside the region or misaligned");
    }
    // The memory comes from mmap and holds an object of this
    // implicit-lifetime type, written by this library in this or another
    // process.
    // NOLINTNEXTLINE(scada-banned,cppcoreguidelines-pro-type-reinterpret-cast): the reviewed helper that maps contract structures onto memory (coding standard, section 5)
    return *reinterpret_cast<Shared*>(region.subspan(offset).data());
}

// The `length` bytes at `offset` in the mapped `region` as text. The view
// refers to the mapping and is valid as long as it is. Only for bytes that
// do not change while the mapping lives (the name pool of a header). Bytes
// outside the region are a defect of this library and end the process.
SCADA_RT [[nodiscard]] inline std::string_view text_at(
    std::span<std::byte> region, std::size_t offset,
    std::size_t length) noexcept {
    if (!fits(region.size(), offset, length)) {
        panic("shared memory: text outside the region");
    }
    // NOLINTNEXTLINE(scada-banned,cppcoreguidelines-pro-type-reinterpret-cast): the reviewed helper that reads mapped bytes as characters (coding standard, section 5)
    return {reinterpret_cast<const char*>(region.subspan(offset).data()),
            length};
}

// Orders the stores of a sequence-lock write (contract, section 6.1): the
// data stores that follow must not pass the store of the odd sequence
// number before it.
//
// ThreadSanitizer does not model fences, and its build runs every atomic
// access in one total order, in which the fence has nothing to add. The
// compiler refuses the call in that build, so it is left out there.
SCADA_RT inline void fence_release() noexcept {
#if !defined(__SANITIZE_THREAD__)
    std::atomic_thread_fence(std::memory_order_release);
#endif
}

// Orders the loads of a sequence-lock read (contract, section 6.2): the
// copies before it must complete before the second load of the sequence
// number. Left out in the ThreadSanitizer build, as `fence_release`.
SCADA_RT inline void fence_acquire() noexcept {
#if !defined(__SANITIZE_THREAD__)
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
}

// Reads a field that another process may be writing.
template <typename Field>
    requires IS_SHARED_ATOMIC<Field>
SCADA_RT [[nodiscard]] Field load_shared(Field& field,
                                         std::memory_order order) noexcept {
    return std::atomic_ref<Field>(field).load(order);
}

// Writes a field that other processes may be reading. The field must lie in
// a mapping that is writable.
template <typename Field>
    requires IS_SHARED_ATOMIC<Field>
SCADA_RT void store_shared(Field& field, std::type_identity_t<Field> value,
                           std::memory_order order) noexcept {
    std::atomic_ref<Field>(field).store(value, order);
}

// Returns a copy of the value of type `Value` stored at `offset` in `bytes`,
// which are private to the caller (an image or a snapshot, not a mapping
// another process writes). A value that does not fit is a defect of this
// library and ends the process.
template <typename Value>
    requires IS_SHARED_STRUCT<Value> || std::is_integral_v<Value>
[[nodiscard]] Value copy_from(std::span<const std::byte> bytes,
                              std::size_t offset) noexcept {
    if (!fits(bytes.size(), offset, sizeof(Value))) {
        panic("shared memory: value outside the bytes");
    }
    std::array<std::byte, sizeof(Value)> representation{};
    std::ranges::copy(bytes.subspan(offset, sizeof(Value)),
                      representation.begin());
    return std::bit_cast<Value>(representation);
}

// Stores `value` at `offset` in `bytes`, with the rules of `copy_from`.
template <typename Value>
    requires IS_SHARED_STRUCT<Value> || std::is_integral_v<Value>
void copy_to(std::span<std::byte> bytes, std::size_t offset,
             const Value& value) noexcept {
    if (!fits(bytes.size(), offset, sizeof(Value))) {
        panic("shared memory: value outside the bytes");
    }
    const auto representation =
        std::bit_cast<std::array<std::byte, sizeof(Value)>>(value);
    std::ranges::copy(representation,
                      bytes.subspan(offset, sizeof(Value)).begin());
}

}  // namespace scada::tag

#endif  // SCADA_TAG_SHARED_MEMORY_H
