#ifndef SCADA_TAG_TESTS_TAG_SET_SAMPLES_H
#define SCADA_TAG_TESTS_TAG_SET_SAMPLES_H

// Tag sets shared by the unit tests of the tag-set definition, its file
// format and the header builder.

#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scada::tag::samples {

// Values of `sample()`. tag_set_json_test.cpp holds the same tag set as a
// document.
inline constexpr std::uint32_t UID_IO = 991;
inline constexpr std::uint32_t UID_CONTROL = 992;
inline constexpr std::uint32_t UID_JOURNAL = 993;
inline constexpr std::uint32_t REALTIME_TIMEOUT_MS = 500;
inline constexpr std::uint32_t IO_RING_CAPACITY = 1024;
inline constexpr std::uint32_t EVENT_QUEUE_CAPACITY = 4096;
// User id of the process of `one_owner()`, and of others that need one.
inline constexpr std::uint32_t UID_OTHER = 1000;

// A small tag set that uses every feature: a process with one owner, one
// with two, one with none; a queue of each kind; a tag of each type and
// every flag. The entries are deliberately not in name order.
inline TagSet sample() {
    TagSet tag_set;
    tag_set.processes = {
        {.name = "journal", .uid = UID_JOURNAL},
        {.name = "io",
         .uid = UID_IO,
         .heartbeat_timeout_ms = REALTIME_TIMEOUT_MS,
         .realtime = true},
        {.name = "control",
         .uid = UID_CONTROL,
         .heartbeat_timeout_ms = REALTIME_TIMEOUT_MS,
         .realtime = true},
    };
    tag_set.owners = {
        {.name = "io",
         .process = "io",
         .change_ring_capacity = IO_RING_CAPACITY},
        {.name = "control.slow", .process = "control"},
        {.name = "control.fast", .process = "control"},
    };
    tag_set.queues = {
        {.name = "result.io.control.fast",
         .producer = "io",
         .consumer = "control"},
        {.name = "events.control.fast",
         .producer = "control",
         .consumer = "journal",
         .capacity = EVENT_QUEUE_CAPACITY},
        {.name = "cmd.io.control.fast",
         .producer = "control",
         .consumer = "io"},
    };
    tag_set.tags = {
        {.name = "plant.pump1.speed",
         .owner = "io",
         .type = TagType::FLOAT64,
         .writable_by_command = true},
        {.name = "plant.pump1.running", .owner = "io", .type = TagType::BOOL},
        {.name = "plant.pump1.mode",
         .owner = "control.slow",
         .type = TagType::INT64,
         .retain = true,
         .retain_now = true},
        {.name = "plant.pump1.starts",
         .owner = "control.fast",
         .type = TagType::UINT64,
         .retain = true},
        {.name = "plant.pump1.label",
         .owner = "control.slow",
         .type = TagType::STRING},
    };
    return tag_set;
}

// One process `p` with its single owner and no tags or queues; tests add
// what they need.
inline TagSet one_owner() {
    TagSet tag_set;
    tag_set.processes = {{.name = "p", .uid = UID_OTHER}};
    tag_set.owners = {{.name = "p", .process = "p"}};
    return tag_set;
}

// Appends `count` BOOL tags named `<prefix><number>` of owner `owner`.
inline void add_tags(TagSet& tag_set, const std::string& owner,
                     const std::string& prefix, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        tag_set.tags.push_back(
            {.name = prefix + std::to_string(i), .owner = owner});
    }
}

// Puts the entries of `entries` into another order: rotated by `step`
// places, and reversed when `step` is odd.
template <typename Definition>
void reorder(std::vector<Definition>& entries, std::size_t step) {
    if (entries.empty()) {
        return;
    }
    const auto middle =
        entries.begin() + static_cast<std::ptrdiff_t>(step % entries.size());
    std::rotate(entries.begin(), middle, entries.end());
    if (step % 2 == 1) {
        std::ranges::reverse(entries);
    }
}

// Returns `tag_set` with every list in another order; each `step` gives a
// different one. The tests need many orders, not random ones.
inline TagSet reordered(TagSet tag_set, std::size_t step) {
    reorder(tag_set.processes, step);
    reorder(tag_set.owners, step + 1);
    reorder(tag_set.queues, step + 2);
    reorder(tag_set.tags, step);
    return tag_set;
}

}  // namespace scada::tag::samples

#endif  // SCADA_TAG_TESTS_TAG_SET_SAMPLES_H
