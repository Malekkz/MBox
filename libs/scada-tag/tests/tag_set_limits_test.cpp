// The count limits of a tag set (contract section 14): sets of up to
// 1,000,001 entries, so a test executable of its own, with its own time
// limit (#164).

#include "scada/tag/tag_set.h"

#include "tag_set_expect.h"
#include "tag_set_samples.h"

#include "scada/common/status.h"
#include "scada/tag/handle.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace {

using namespace scada::tag;
using scada::StatusCode;
using scada::tag::tag_set_expect::expect_rejected;
using scada::tag::tag_set_expect::expect_valid;

// Contract section 14: number of processes, owners, queues and tags.
TEST(TagSetValidate, AppliesTheCountLimits) {
    {
        TagSet tag_set;
        for (std::size_t i = 0; i < MAX_PROCESSES; ++i) {
            tag_set.processes.push_back(
                {.name = "p" + std::to_string(i), .uid = 1});
        }
        expect_valid(tag_set);
        tag_set.processes.push_back({.name = "one-too-many", .uid = 1});
        expect_rejected(tag_set, StatusCode::OUT_OF_RANGE,
                        "the tag set has 1025 process entries; the limit is "
                        "1024");
    }
    {
        TagSet tag_set;
        tag_set.processes = {{.name = "p", .uid = 1}};
        for (std::size_t i = 0; i < MAX_OWNERS; ++i) {
            tag_set.owners.push_back(
                {.name = "p.w" + std::to_string(i), .process = "p"});
        }
        expect_valid(tag_set);
        tag_set.owners.push_back({.name = "p.one-too-many", .process = "p"});
        expect_rejected(tag_set, StatusCode::OUT_OF_RANGE,
                        "the tag set has 1024 owner entries; the limit is "
                        "1023");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.processes.push_back({.name = "c", .uid = samples::UID_OTHER});
        for (std::size_t i = 0; i < MAX_QUEUES; ++i) {
            tag_set.queues.push_back({.name = "events.q" + std::to_string(i),
                                      .producer = "p",
                                      .consumer = "c"});
        }
        expect_valid(tag_set);
        tag_set.queues.push_back(
            {.name = "events.one-too-many", .producer = "p", .consumer = "c"});
        expect_rejected(tag_set, StatusCode::OUT_OF_RANGE,
                        "the tag set has 4097 queue entries; the limit is "
                        "4096");
    }
    {
        // The count is checked before anything else, so the entries need no
        // content. The largest valid tag set is built in the performance
        // test, where it does not slow the unit tests down.
        TagSet tag_set = samples::one_owner();
        tag_set.tags.resize(MAX_TAGS + 1);
        expect_rejected(tag_set, StatusCode::OUT_OF_RANGE,
                        "the tag set has 1000001 tag entries; the limit is "
                        "1000000");
    }
}

}  // namespace
