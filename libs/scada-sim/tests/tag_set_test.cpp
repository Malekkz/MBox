// Unit tests for the tag set of a simulation: what it holds, that it is
// valid, and that its file reads back as the same tag set.

#include "scada/sim/tag_set.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/tag_set_json.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace scada::sim;
using scada::Result;
using scada::StatusCode;
using scada::tag::TagSet;
using scada::tag::TagType;

constexpr std::uint32_t UID = 1000;
constexpr std::uint32_t TAGD_UID = 999;
constexpr std::uint64_t SMALL_TAGS = 10;
// The reference load.
constexpr std::uint32_t REFERENCE_OWNERS = 10;
constexpr std::uint64_t REFERENCE_TAGS = 50'000;
// 50,000 tags per owner need a ring of 131,072 entries.
constexpr std::uint64_t LARGE_TAGS = 100'000;
constexpr std::uint32_t LARGE_RING = 131'072;
constexpr std::uint32_t SMALL_RING = scada::tag::MIN_CHANGE_RING_CAPACITY;
// Settings that differ from the defaults.
constexpr std::uint32_t CHECK_MS = 50;
constexpr std::uint32_t TIMEOUT_MS = 700;
constexpr std::uint32_t RING = 2048;

TagSetOptions small() {
    TagSetOptions options;
    options.layout = Layout{.prefix = "sim",
                            .processes = 2,
                            .owners = 4,
                            .tags = SMALL_TAGS,
                            .patterns = {Pattern::CHECK, Pattern::SINE}};
    options.uid = UID;
    options.tagd_uid = TAGD_UID;
    options.readers = {"reader"};
    return options;
}

// The tag set of `options`, or nothing with a failed expectation.
std::optional<TagSet> made(const TagSetOptions& options) {
    std::string detail;
    Result<TagSet> tag_set = make_tag_set(options, detail);
    if (!tag_set.ok()) {
        ADD_FAILURE() << detail;
        return std::nullopt;
    }
    return std::move(tag_set).value();
}

TEST(SimTagSet, HoldsTagdTheSimulatorProcessesAndTheReaders) {
    const std::optional<TagSet> tag_set = made(small());
    if (!tag_set.has_value()) {
        return;
    }
    std::vector<std::string> names;
    std::vector<std::uint32_t> uids;
    for (const scada::tag::ProcessDefinition& process : tag_set->processes) {
        names.push_back(process.name);
        uids.push_back(process.uid);
    }
    EXPECT_EQ(names,
              (std::vector<std::string>{"tagd", "sim0", "sim1", "reader"}));
    EXPECT_EQ(uids, (std::vector<std::uint32_t>{TAGD_UID, UID, UID, UID}));
    EXPECT_TRUE(tag_set->queues.empty());
}

TEST(SimTagSet, HoldsTheOwnersOfTheLayout) {
    const std::optional<TagSet> tag_set = made(small());
    if (!tag_set.has_value()) {
        return;
    }
    std::vector<std::string> owners;
    for (const scada::tag::OwnerDefinition& owner : tag_set->owners) {
        owners.push_back(owner.name + " of " + owner.process);
        EXPECT_EQ(owner.change_ring_capacity,
                  scada::tag::DEFAULT_CHANGE_RING_CAPACITY);
    }
    EXPECT_EQ(owners,
              (std::vector<std::string>{"sim0.w0 of sim0", "sim0.w1 of sim0",
                                        "sim1.w0 of sim1", "sim1.w1 of sim1"}));
}

TEST(SimTagSet, HoldsTheTagsOfTheLayout) {
    const std::optional<TagSet> tag_set = made(small());
    if (!tag_set.has_value()) {
        return;
    }
    ASSERT_EQ(tag_set->tags.size(), SMALL_TAGS);
    EXPECT_EQ(tag_set->tags[0].name, "sim0.w0.t0");
    EXPECT_EQ(tag_set->tags[0].type, TagType::UINT64);
    EXPECT_EQ(tag_set->tags[1].name, "sim0.w0.t1");
    EXPECT_EQ(tag_set->tags[1].type, TagType::FLOAT64);
    EXPECT_EQ(tag_set->tags.back().name, "sim1.w1.t1");
}

TEST(SimTagSet, TheReferenceLoadIsValid) {
    TagSetOptions options;
    options.layout = Layout{.prefix = "load",
                            .processes = 1,
                            .owners = REFERENCE_OWNERS,
                            .tags = REFERENCE_TAGS,
                            .patterns = {Pattern::CHECK}};
    const std::optional<TagSet> tag_set = made(options);
    if (!tag_set.has_value()) {
        return;
    }
    EXPECT_EQ(tag_set->tags.size(), REFERENCE_TAGS);
    EXPECT_EQ(tag_set->owners.size(), REFERENCE_OWNERS);
}

TEST(SimTagSet, ARingIsAtLeastTwiceTheTagsOfItsOwner) {
    TagSetOptions options = small();
    options.layout.owners = 2;
    options.layout.tags = LARGE_TAGS;
    const std::optional<TagSet> tag_set = made(options);
    if (!tag_set.has_value()) {
        return;
    }
    EXPECT_EQ(tag_set->owners[0].change_ring_capacity, LARGE_RING);

    options.ring_capacity = SMALL_RING;
    std::string detail;
    EXPECT_EQ(make_tag_set(options, detail).status().code(),
              StatusCode::OUT_OF_RANGE);
    EXPECT_FALSE(detail.empty());
}

TEST(SimTagSet, RefusesAReaderThatCollidesWithAProcess) {
    TagSetOptions options = small();
    options.readers = {"sim0"};
    std::string detail;
    EXPECT_EQ(make_tag_set(options, detail).status().code(),
              StatusCode::ALREADY_EXISTS);
}

TEST(SimTagSet, RefusesAnInvalidLayout) {
    TagSetOptions options = small();
    options.layout.owners = 3;
    std::string detail;
    EXPECT_EQ(make_tag_set(options, detail).status().code(),
              StatusCode::INVALID_ARGUMENT);
}

// `options` as a file, read back; nothing with a failed expectation.
std::optional<TagSet> read_back(const TagSet& tag_set) {
    std::string detail;
    const std::string text = tag_set_json(tag_set);
    Result<TagSet> parsed = scada::tag::parse_tag_set_json(text, detail);
    if (!parsed.ok()) {
        ADD_FAILURE() << detail << "\n" << text;
        return std::nullopt;
    }
    return std::move(parsed).value();
}

TagSetOptions with_other_settings() {
    TagSetOptions options = small();
    options.heartbeat_check_ms = CHECK_MS;
    options.heartbeat_timeout_ms = TIMEOUT_MS;
    options.ring_capacity = RING;
    return options;
}

TEST(SimTagSet, TheFileKeepsTheSettings) {
    const std::optional<TagSet> tag_set = made(with_other_settings());
    if (!tag_set.has_value()) {
        return;
    }
    const std::optional<TagSet> back = read_back(*tag_set);
    if (!back.has_value()) {
        return;
    }
    EXPECT_EQ(back->heartbeat_check_ms, CHECK_MS);
    ASSERT_EQ(back->processes.size(), tag_set->processes.size());
    EXPECT_EQ(back->processes[1].heartbeat_timeout_ms, TIMEOUT_MS);
    ASSERT_EQ(back->owners.size(), tag_set->owners.size());
    EXPECT_EQ(back->owners[0].change_ring_capacity, RING);
}

TEST(SimTagSet, TheFileReadsBackWithTheSameHash) {
    const std::optional<TagSet> tag_set = made(with_other_settings());
    if (!tag_set.has_value()) {
        return;
    }
    const std::optional<TagSet> back = read_back(*tag_set);
    if (!back.has_value()) {
        return;
    }
    std::size_t different = 0;
    for (std::size_t i = 0; i < back->tags.size(); ++i) {
        if (back->tags[i].name != tag_set->tags[i].name ||
            back->tags[i].type != tag_set->tags[i].type) {
            ++different;
        }
    }
    EXPECT_EQ(back->tags.size(), tag_set->tags.size());
    EXPECT_EQ(different, 0U);
    std::string detail;
    const auto hash = scada::tag::config_hash(*tag_set, detail);
    const auto hash_back = scada::tag::config_hash(*back, detail);
    if (!hash.ok() || !hash_back.ok()) {
        FAIL() << detail;
    }
    EXPECT_EQ(hash.value(), hash_back.value());
}

}  // namespace
