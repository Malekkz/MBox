// Fuzz target for the checks of reliable queue records (contract, section
// 9.4). A consumer reads every record from a region another process
// writes, so its bytes are outside input. The input is taken as a record of
// each kind; a record that passes is checked against the rules on its
// fields again, written another way, and must survive the round trip
// through its structure unchanged. The last 8 bytes, if any, are a time for
// the expiry check, which must not overflow on any pair of times.

#include "layout.h"
#include "queue_region.h"

#include "scada/common/panic.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {

using scada::tag::EventRecord;
using scada::tag::RecordWords;
using scada::tag::WriteRequest;
using scada::tag::WriteResult;

constexpr std::uint8_t LAST_OPERATION = 5;
constexpr std::uint8_t LAST_SOURCE = 3;
constexpr std::uint8_t LAST_STATUS = 7;
constexpr std::uint16_t LAST_EVENT_KIND = 4;
constexpr std::uint8_t OPERATION_OPERATE = 3;
constexpr std::uint8_t OPERATION_CANCEL = 4;
constexpr std::uint8_t OPERATION_READ_NOW = 5;
constexpr std::uint8_t SOURCE_OPERATOR = 2;
constexpr std::uint8_t TYPE_BOOL = 1;
constexpr std::uint8_t TYPE_FLOAT64 = 4;

// The first `size` bytes of `words` equal those of `other`.
bool same_prefix(const RecordWords& words, const RecordWords& other,
                 std::size_t size) {
    const auto left =
        std::bit_cast<std::array<std::byte, sizeof(words)>>(words);
    const auto right =
        std::bit_cast<std::array<std::byte, sizeof(other)>>(other);
    return std::ranges::equal(std::span(left).first(size),
                              std::span(right).first(size));
}

bool event_rules(const EventRecord& record) {
    return record.kind >= 1 && record.kind <= LAST_EVENT_KIND &&
           record.payload_len <= scada::tag::EVENT_PAYLOAD_SIZE;
}

bool request_rules(const WriteRequest& record) {
    const bool reserved_zero =
        record.reserved0 == 0 && record.reserved1 == 0 &&
        std::ranges::count(record.reserved, 0) ==
            static_cast<std::ptrdiff_t>(record.reserved.size());
    const bool no_value = record.operation == OPERATION_CANCEL ||
                          record.operation == OPERATION_READ_NOW;
    const bool value_ok =
        record.value_type == 0
            ? no_value && record.value == 0
            : record.value_type <= TYPE_FLOAT64 &&
                  (record.value_type != TYPE_BOOL || record.value <= 1);
    const bool select = record.operation == OPERATION_OPERATE ||
                        record.operation == OPERATION_CANCEL;
    return reserved_zero && record.operation >= 1 &&
           record.operation <= LAST_OPERATION && record.source >= 1 &&
           record.source <= LAST_SOURCE && value_ok &&
           (record.source == SOURCE_OPERATOR || record.user_id == 0) &&
           select == (record.select_request_id != 0);
}

bool result_rules(const WriteResult& record) {
    return record.status >= 1 && record.status <= LAST_STATUS &&
           std::ranges::count(record.reserved0, 0) == 3 &&
           std::ranges::count(record.reserved, 0) ==
               static_cast<std::ptrdiff_t>(record.reserved.size());
}

// True if `later` lies more than the lifetime limit after `earlier`.
bool beyond(std::int64_t later, std::int64_t earlier) {
    constexpr std::int64_t LIMIT = scada::tag::MAX_REQUEST_LIFETIME_NS;
    if (earlier > std::numeric_limits<std::int64_t>::max() - LIMIT) {
        return false;  // nothing lies that far after `earlier`
    }
    return later > earlier + LIMIT;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    std::array<std::byte, sizeof(RecordWords)> bytes{};
    const std::size_t used = std::min(input.size(), bytes.size());
    std::ranges::transform(input.first(used), bytes.begin(),
                           [](std::uint8_t byte) { return std::byte{byte}; });
    const RecordWords words{
        std::bit_cast<std::array<std::uint64_t, scada::tag::MAX_RECORD_WORDS>>(
            bytes)};

    const EventRecord event = scada::tag::event_of(words);
    if (scada::tag::event_valid(event) != event_rules(event) ||
        !same_prefix(scada::tag::words_of(event), words, sizeof(EventRecord))) {
        scada::panic("event record check or round trip differs");
    }
    const WriteRequest request = scada::tag::request_of(words);
    if (scada::tag::request_valid(request) != request_rules(request) ||
        !same_prefix(scada::tag::words_of(request), words,
                     sizeof(WriteRequest))) {
        scada::panic("write request check or round trip differs");
    }
    const WriteResult result = scada::tag::result_of(words);
    if (scada::tag::result_valid(result) != result_rules(result) ||
        !same_prefix(scada::tag::words_of(result), words,
                     sizeof(WriteResult))) {
        scada::panic("write result check or round trip differs");
    }

    std::int64_t now = 0;
    if (input.size() >= sizeof(now)) {
        std::array<std::uint8_t, sizeof(now)> last{};
        std::ranges::copy(input.last(sizeof(now)), last.begin());
        now = std::bit_cast<std::int64_t>(last);
    }
    // The five reasons of decision D-38, judged by additions that are
    // guarded against overflow instead of by differences.
    const std::int64_t expiry = request.expiry_mono_ns;
    const std::int64_t posted = request.posted_mono_ns;
    const bool should = expiry <= now || posted > now || expiry < posted ||
                        beyond(expiry, posted) || beyond(expiry, now);
    if (scada::tag::request_expired(request, now) != should) {
        scada::panic("the expiry check differs");
    }
    return 0;
}
