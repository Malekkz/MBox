// Unit tests for the tag-set definition: what `validate` rejects, with which
// message, and that `config_hash` depends on the content of a tag set but
// not on the order of its entries.

#include "scada/tag/tag_set.h"

#include "tag_set_samples.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Result;
using scada::Status;
using scada::StatusCode;

// Expects that `tag_set` is rejected with `code` and a detail that contains
// `fragment`.
void expect_rejected(const TagSet& tag_set, StatusCode code,
                     std::string_view fragment) {
    std::string detail;
    const Status status = validate(tag_set, detail);
    EXPECT_EQ(status.code(), code) << detail;
    EXPECT_NE(detail.find(fragment), std::string::npos)
        << "detail: " << detail << "\nexpected to contain: " << fragment;
    // The hash refuses the same tag set for the same reason.
    std::string hash_detail;
    const Result<ConfigHash> hash = config_hash(tag_set, hash_detail);
    EXPECT_EQ(hash.status().code(), code);
    EXPECT_EQ(hash_detail, detail);
}

void expect_valid(const TagSet& tag_set) {
    std::string detail = "untouched";
    EXPECT_TRUE(validate(tag_set, detail).ok()) << detail;
    EXPECT_EQ(detail, "untouched");
}

ConfigHash hash_of(const TagSet& tag_set) {
    std::string detail;
    const Result<ConfigHash> hash = config_hash(tag_set, detail);
    EXPECT_TRUE(hash.ok()) << detail;
    return hash.ok() ? hash.value() : ConfigHash{};
}

std::string hex(const ConfigHash& hash) {
    constexpr std::string_view DIGITS = "0123456789abcdef";
    constexpr unsigned NIBBLE_BITS = 4;
    constexpr unsigned NIBBLE_MASK = 0xF;
    std::string text;
    for (const std::uint8_t byte : hash) {
        text.push_back(DIGITS.at(byte >> NIBBLE_BITS));
        text.push_back(DIGITS.at(byte & NIBBLE_MASK));
    }
    return text;
}

TEST(TagSetNames, FollowTheGrammarOfTheContract) {
    constexpr std::size_t MAX = 5;
    EXPECT_TRUE(is_valid_name("a", MAX));
    EXPECT_TRUE(is_valid_name("az09_", MAX));
    EXPECT_TRUE(is_valid_name("a.b-c", MAX));
    EXPECT_FALSE(is_valid_name("", MAX));
    EXPECT_FALSE(is_valid_name("abcdef", MAX));
    EXPECT_FALSE(is_valid_name("A", MAX));
    EXPECT_FALSE(is_valid_name("a b", MAX));
    EXPECT_FALSE(is_valid_name("a/b", MAX));
    EXPECT_FALSE(is_valid_name("\xC3\xA9", MAX));
    EXPECT_FALSE(is_valid_name(std::string_view("a\0b", 3), MAX));
}

TEST(TagSetValidate, AcceptsTheSampleAndTheEmptyTagSet) {
    expect_valid(samples::sample());
    expect_valid(TagSet{});
}

TEST(TagSetValidate, AcceptsAProcessWithoutOwnerAndAnOwnerWithoutTags) {
    TagSet tag_set = samples::one_owner();
    tag_set.processes.push_back({.name = "api", .uid = samples::UID_OTHER});
    expect_valid(tag_set);
}

TEST(TagSetValidate, RejectsDuplicateNames) {
    {
        TagSet tag_set = samples::sample();
        tag_set.tags.push_back(
            {.name = "plant.pump1.speed", .owner = "control.fast"});
        expect_rejected(tag_set, StatusCode::ALREADY_EXISTS,
                        "tag 'plant.pump1.speed': the name is used more than "
                        "once");
    }
    {
        TagSet tag_set = samples::sample();
        tag_set.owners.push_back({.name = "io", .process = "journal"});
        expect_rejected(tag_set, StatusCode::ALREADY_EXISTS, "owner 'io'");
    }
    {
        TagSet tag_set = samples::sample();
        tag_set.processes.push_back(
            {.name = "journal", .uid = samples::UID_OTHER});
        expect_rejected(tag_set, StatusCode::ALREADY_EXISTS,
                        "process 'journal'");
    }
    {
        TagSet tag_set = samples::sample();
        tag_set.queues.push_back({.name = "events.control.fast",
                                  .producer = "io",
                                  .consumer = "journal"});
        expect_rejected(tag_set, StatusCode::ALREADY_EXISTS,
                        "queue 'events.control.fast'");
    }
}

TEST(TagSetValidate, RejectsUnknownOwnersAndProcesses) {
    {
        TagSet tag_set = samples::sample();
        tag_set.tags.push_back({.name = "orphan", .owner = "nobody"});
        expect_rejected(tag_set, StatusCode::NOT_FOUND,
                        "tag 'orphan': owner 'nobody' is not an owner of the "
                        "tag set");
    }
    {
        // A process is not an owner unless it is declared as one.
        TagSet tag_set = samples::sample();
        tag_set.tags.push_back({.name = "orphan", .owner = "journal"});
        expect_rejected(tag_set, StatusCode::NOT_FOUND, "owner 'journal'");
    }
    {
        // The owner text is not echoed when it is not a name.
        TagSet tag_set = samples::sample();
        tag_set.tags.push_back({.name = "orphan", .owner = "No Body\n"});
        expect_rejected(tag_set, StatusCode::NOT_FOUND,
                        "tag 'orphan': the owner is not a valid owner name");
    }
    {
        TagSet tag_set = samples::sample();
        tag_set.owners.push_back({.name = "ghost", .process = "ghost"});
        expect_rejected(tag_set, StatusCode::NOT_FOUND,
                        "owner 'ghost': process 'ghost' is not a process of "
                        "the tag set");
    }
    {
        TagSet tag_set = samples::sample();
        tag_set.queues.push_back(
            {.name = "events.x", .producer = "ghost", .consumer = "journal"});
        expect_rejected(tag_set, StatusCode::NOT_FOUND,
                        "queue 'events.x': producer 'ghost'");
    }
    {
        TagSet tag_set = samples::sample();
        tag_set.queues.push_back(
            {.name = "events.x", .producer = "io", .consumer = "ghost"});
        expect_rejected(tag_set, StatusCode::NOT_FOUND,
                        "queue 'events.x': consumer 'ghost'");
    }
}

TEST(TagSetValidate, RejectsNamesOutsideTheGrammarOrTooLong) {
    {
        TagSet tag_set = samples::one_owner();
        tag_set.tags.push_back({.name = "Upper", .owner = "p"});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "tag at position 0 (from 0): the name must be 1 to "
                        "127 bytes of [a-z0-9_.-]");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.tags.push_back({.name = "", .owner = "p"});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "tag at position 0");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.tags.push_back(
            {.name = std::string(MAX_TAG_NAME_LENGTH, 'a'), .owner = "p"});
        expect_valid(tag_set);
        tag_set.tags.push_back(
            {.name = std::string(MAX_TAG_NAME_LENGTH + 1, 'a'), .owner = "p"});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "tag at position 1");
    }
    {
        const std::string longest(MAX_SHORT_NAME_LENGTH, 'p');
        TagSet tag_set;
        tag_set.processes = {{.name = longest, .uid = 1}};
        tag_set.owners = {{.name = longest, .process = longest}};
        expect_valid(tag_set);
        tag_set.processes.push_back({.name = longest + "p", .uid = 1});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "process at position 1 (from 0): the name must be 1 "
                        "to 31 bytes");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.queues.push_back(
            {.name = "events." + std::string(MAX_SHORT_NAME_LENGTH, 'q'),
             .producer = "p",
             .consumer = "p"});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "queue at position 0");
    }
}

// Contract 1.1, section 2: an owner is named `<process>` or
// `<process>.<writer>` with a writer that is not empty, however many owners
// its process has.
TEST(TagSetValidate, AppliesTheOwnerNamingRule) {
    {
        // The only owner of a process named `<process>.<writer>`: refused
        // under 1.0.
        TagSet tag_set = samples::one_owner();
        tag_set.owners.front().name = "p.writer";
        expect_valid(tag_set);
    }
    {
        // The plain owner beside a `<process>.<writer>` owner: refused
        // under 1.0.
        TagSet tag_set = samples::one_owner();
        tag_set.owners.push_back({.name = "p.second", .process = "p"});
        expect_valid(tag_set);
        tag_set.owners.push_back({.name = "p.third.part", .process = "p"});
        expect_valid(tag_set);
    }
    {
        // A foreign prefix, an empty writer and a name that only starts
        // like the process stay refused, beside other owners and alone.
        for (const bool alone : {false, true}) {
            for (const std::string_view name : {"px.b", "p.", "px", "q"}) {
                TagSet tag_set = samples::one_owner();
                tag_set.processes.push_back(
                    {.name = "q", .uid = samples::UID_OTHER});
                if (alone) {
                    tag_set.owners.clear();
                }
                tag_set.owners.push_back(
                    {.name = std::string(name), .process = "p"});
                expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                                "owner '" + std::string(name) +
                                    "': an owner of process 'p' must be "
                                    "named 'p' or <process>.<writer> with a "
                                    "writer that is not empty");
            }
        }
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.owners = {{.name = "p.a", .process = "p"},
                          {.name = "p.b", .process = "p"}};
        expect_valid(tag_set);
    }
}

TEST(TagSetValidate, RejectsQueuesWithoutAKindPrefixOrPowerOfTwoCapacity) {
    constexpr std::uint32_t CAPACITY = 1024;
    constexpr std::uint32_t NOT_A_POWER_OF_TWO = 1000;
    const auto with_queue = [](std::string name, std::uint32_t capacity) {
        TagSet tag_set = samples::one_owner();
        tag_set.processes.push_back({.name = "c", .uid = samples::UID_OTHER});
        tag_set.queues.push_back({.name = std::move(name),
                                  .producer = "p",
                                  .consumer = "c",
                                  .capacity = capacity});
        return tag_set;
    };
    expect_valid(with_queue("events.p", 1));
    TagSet command = with_queue("cmd.c.p", MAX_QUEUE_CAPACITY);
    command.queues.push_back(
        {.name = "result.c.p", .producer = "c", .consumer = "p"});
    expect_valid(command);
    expect_valid(with_queue("result.p.q", CAPACITY));
    expect_rejected(with_queue("alarms.p", CAPACITY),
                    StatusCode::INVALID_ARGUMENT,
                    "queue 'alarms.p': the name must start with events., cmd. "
                    "or result.");
    expect_rejected(with_queue("events.", CAPACITY),
                    StatusCode::INVALID_ARGUMENT, "queue 'events.'");
    expect_rejected(with_queue("events", CAPACITY),
                    StatusCode::INVALID_ARGUMENT, "queue 'events'");
    expect_rejected(with_queue("events.p", 0), StatusCode::INVALID_ARGUMENT,
                    "queue 'events.p': capacity 0 must be a power of two");
    expect_rejected(with_queue("events.p", NOT_A_POWER_OF_TWO),
                    StatusCode::INVALID_ARGUMENT, "capacity 1000");
    TagSet one_process = with_queue("events.p", CAPACITY);
    one_process.queues.front().consumer = "p";
    expect_rejected(one_process, StatusCode::INVALID_ARGUMENT,
                    "queue 'events.p': producer and consumer must be "
                    "different processes");
}

// Issue #52: a command needs a way back (reference 5.3 and 8.2, step 10), so
// every `cmd.<x>` queue has the answer queue `result.<x>` of contract 9.2,
// with producer and consumer swapped.
TEST(TagSetValidate, RequiresTheResultQueueOfEveryCommandQueue) {
    const auto with_queues = [](std::vector<QueueDefinition> queues) {
        TagSet tag_set = samples::one_owner();
        tag_set.processes.push_back({.name = "c", .uid = samples::UID_OTHER});
        tag_set.processes.push_back({.name = "d", .uid = samples::UID_OTHER});
        tag_set.queues = std::move(queues);
        return tag_set;
    };
    const QueueDefinition command{
        .name = "cmd.c.p", .producer = "p", .consumer = "c"};
    expect_valid(with_queues(
        {command, {.name = "result.c.p", .producer = "c", .consumer = "p"}}));
    // A result queue without a command queue is allowed.
    expect_valid(with_queues(
        {{.name = "result.c.p", .producer = "c", .consumer = "p"}}));
    expect_rejected(with_queues({command}), StatusCode::INVALID_ARGUMENT,
                    "queue 'cmd.c.p': needs the queue 'result.c.p' for its "
                    "results, from 'c' to 'p'");
    // The answer queue is found by name only: a result queue of another name
    // between the same processes does not count.
    expect_rejected(
        with_queues({command,
                     {.name = "result.c.q", .producer = "c", .consumer = "p"}}),
        StatusCode::INVALID_ARGUMENT,
        "queue 'cmd.c.p': needs the queue 'result.c.p'");
    // An events queue of the right name is not a result queue.
    expect_rejected(
        with_queues({command,
                     {.name = "events.c.p", .producer = "c", .consumer = "p"}}),
        StatusCode::INVALID_ARGUMENT,
        "queue 'cmd.c.p': needs the queue 'result.c.p'");
    const std::vector<QueueDefinition> wrong_ends{
        {.name = "result.c.p", .producer = "p", .consumer = "c"},
        {.name = "result.c.p", .producer = "c", .consumer = "d"},
        {.name = "result.c.p", .producer = "d", .consumer = "p"},
    };
    for (const QueueDefinition& result : wrong_ends) {
        expect_rejected(with_queues({command, result}),
                        StatusCode::INVALID_ARGUMENT,
                        "queue 'cmd.c.p': its result queue 'result.c.p' must "
                        "go from 'c' to 'p'");
    }
    // A name that leaves no room for the prefix `result.` cannot be paired.
    constexpr std::string_view CMD_PREFIX = "cmd.";
    const std::string longest =
        std::string(CMD_PREFIX) +
        std::string(MAX_SHORT_NAME_LENGTH - CMD_PREFIX.size(), 'x');
    expect_rejected(
        with_queues({{.name = longest, .producer = "p", .consumer = "c"}}),
        StatusCode::INVALID_ARGUMENT, "needs the queue 'result.xxx");
}

TEST(TagSetValidate, RejectsInvalidValuesAndFlagCombinations) {
    {
        TagSet tag_set = samples::one_owner();
        tag_set.heartbeat_check_ms = 0;
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "heartbeat_check_ms must be at least 1");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.processes.front().heartbeat_timeout_ms = 0;
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "process 'p': heartbeat_timeout_ms must be at least 1");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.processes.front().uid = MAX_UID;
        expect_valid(tag_set);
        tag_set.processes.front().uid = MAX_UID + 1;
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "process 'p': uid 4294967295 is not a user id");
    }
    {
        // Its retain file would look like a temporary one (section 12).
        TagSet tag_set = samples::one_owner();
        tag_set.processes.front().name = ".tmp";
        tag_set.owners.front().name = ".tmp";
        tag_set.owners.front().process = ".tmp";
        tag_set.tags.clear();
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "owner '.tmp': an owner name cannot start with '.'");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.tags.push_back({.name = "t", .owner = "p", .retain_now = true});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "tag 't': retain_now needs retain");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.tags.push_back({.name = "t",
                                .owner = "p",
                                .type = TagType::STRING,
                                .writable_by_command = true});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "tag 't': a string tag cannot be writable_by_command");
    }
    {
        TagSet tag_set = samples::one_owner();
        tag_set.tags.push_back({.name = "t",
                                .owner = "p",
                                .type = TagType::STRING,
                                .retain = true});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "tag 't': a string tag cannot be retained");
    }
    {
        constexpr std::uint8_t NOT_A_TYPE = 6;
        TagSet tag_set = samples::one_owner();
        tag_set.tags.push_back({.name = "t",
                                .owner = "p",
                                .type = static_cast<TagType>(NOT_A_TYPE)});
        expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                        "tag 't': the type is not a tag type");
    }
}

// Contract section 14: change ring capacity.
TEST(TagSetValidate, AppliesTheChangeRingLimits) {
    constexpr std::uint32_t NOT_A_POWER_OF_TWO = 1000;
    constexpr std::size_t HALF_OF_MINIMUM = MIN_CHANGE_RING_CAPACITY / 2;
    const auto with_capacity = [](std::uint32_t capacity, std::size_t tags) {
        TagSet tag_set = samples::one_owner();
        tag_set.owners.front().change_ring_capacity = capacity;
        samples::add_tags(tag_set, "p", "t", tags);
        return tag_set;
    };
    expect_valid(with_capacity(MIN_CHANGE_RING_CAPACITY, 0));
    expect_valid(with_capacity(MAX_CHANGE_RING_CAPACITY, 0));
    expect_rejected(with_capacity(MIN_CHANGE_RING_CAPACITY / 2, 0),
                    StatusCode::OUT_OF_RANGE,
                    "owner 'p': change_ring_capacity 512 must be a power of "
                    "two from 1024 to 1048576");
    expect_rejected(with_capacity(MAX_CHANGE_RING_CAPACITY * 2, 0),
                    StatusCode::OUT_OF_RANGE, "change_ring_capacity 2097152");
    expect_rejected(with_capacity(NOT_A_POWER_OF_TWO, 0),
                    StatusCode::OUT_OF_RANGE, "change_ring_capacity 1000");
    // At least twice the owner's tag count.
    expect_valid(with_capacity(MIN_CHANGE_RING_CAPACITY, HALF_OF_MINIMUM));
    expect_rejected(
        with_capacity(MIN_CHANGE_RING_CAPACITY, HALF_OF_MINIMUM + 1),
        StatusCode::OUT_OF_RANGE,
        "owner 'p': change_ring_capacity 1024 is less than twice "
        "its 513 tags");
}

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

// The hash is the SHA-256 of the canonical encoding described in the
// README. The expected value was computed from that description with a
// separate program, so a change of the encoding fails here.
TEST(TagSetHash, IsTheHashOfTheCanonicalEncoding) {
    EXPECT_EQ(
        hex(hash_of(samples::sample())),
        "e1e7f39b7000624cf7715d4cf6ceee236e9d03d0f3b0adfc5f2bc218999e93aa");
}

// Contract 1.1 changes only which owner names are accepted: a tag set valid
// under 1.0 is valid under 1.1 with the same `config_hash` (#151). The
// values were computed by the 1.0 library (commit 0bc8052) for these
// tag sets: the plain owner alone, `<process>.<writer>` owners only, and the
// sample, which has both kinds in different processes.
TEST(TagSetHash, ATagSetValidUnderContract10KeepsItsHash) {
    TagSet plain = samples::one_owner();
    samples::add_tags(plain, "p", "p.t", 2);

    TagSet writers;
    writers.processes = {{.name = "p", .uid = samples::UID_OTHER}};
    writers.owners = {{.name = "p.b", .process = "p"},
                      {.name = "p.a", .process = "p"}};
    samples::add_tags(writers, "p.a", "p.a.t", 1);
    samples::add_tags(writers, "p.b", "p.b.t", 1);

    expect_valid(plain);
    expect_valid(writers);
    EXPECT_EQ(
        hex(hash_of(plain)),
        "57e592d2d35c9a0d0c41c7653208515bdcda8b7aee0fb75bdc91317216f68ff0");
    EXPECT_EQ(
        hex(hash_of(writers)),
        "e3a63ec04f39d91f7cd57c730be9450cc4ecb698e88548d91a427f560035de95");
    EXPECT_EQ(
        hex(hash_of(samples::sample())),
        "e1e7f39b7000624cf7715d4cf6ceee236e9d03d0f3b0adfc5f2bc218999e93aa");
}

TEST(TagSetHash, DoesNotDependOnTheOrderOfTheEntries) {
    const TagSet original = samples::sample();
    const ConfigHash expected = hash_of(original);

    TagSet reversed = original;
    std::ranges::reverse(reversed.processes);
    std::ranges::reverse(reversed.owners);
    std::ranges::reverse(reversed.queues);
    std::ranges::reverse(reversed.tags);
    EXPECT_EQ(hash_of(reversed), expected);

    constexpr std::size_t ORDERS = 20;
    for (std::size_t step = 1; step <= ORDERS; ++step) {
        EXPECT_EQ(hash_of(samples::reordered(original, step)), expected)
            << "order " << step;
    }
}

// Values that differ from those of the sample.
constexpr std::uint32_t OTHER_CHECK_MS = 251;
constexpr std::uint32_t OTHER_TIMEOUT_MS = 2999;
constexpr std::uint32_t OTHER_CAPACITY = 2048;

// Every field of the definition is part of the hash.
TEST(TagSetHash, ChangesWithEveryFieldOfTheDefinition) {
    const std::vector<std::function<void(TagSet&)>> changes{
        [](TagSet& set) { set.heartbeat_check_ms = OTHER_CHECK_MS; },
        [](TagSet& set) { set.processes.at(0).uid = 1; },
        [](TagSet& set) {
            set.processes.at(0).heartbeat_timeout_ms = OTHER_TIMEOUT_MS;
        },
        [](TagSet& set) { set.processes.at(0).realtime = true; },
        [](TagSet& set) {
            set.processes.push_back({.name = "api", .uid = samples::UID_OTHER});
        },
        [](TagSet& set) {
            set.owners.at(0).change_ring_capacity = OTHER_CAPACITY;
        },
        [](TagSet& set) { set.queues.at(0).capacity = OTHER_CAPACITY; },
        [](TagSet& set) { set.queues.at(1).consumer = "io"; },
        [](TagSet& set) { set.queues.at(1).producer = "io"; },
        [](TagSet& set) { set.queues.at(1).name = "events.control.slow"; },
        [](TagSet& set) { set.tags.at(0).name = "plant.pump1.speed2"; },
        [](TagSet& set) { set.tags.at(0).owner = "control.fast"; },
        [](TagSet& set) { set.tags.at(0).type = TagType::INT64; },
        [](TagSet& set) { set.tags.at(0).retain = true; },
        [](TagSet& set) { set.tags.at(0).writable_by_command = false; },
        [](TagSet& set) { set.tags.at(3).retain_now = true; },
        [](TagSet& set) { set.tags.pop_back(); },
    };
    std::vector<std::string> seen{hex(hash_of(samples::sample()))};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        TagSet changed = samples::sample();
        changes.at(i)(changed);
        const std::string hash = hex(hash_of(changed));
        EXPECT_EQ(std::ranges::count(seen, hash), 0) << "change " << i;
        seen.push_back(hash);
    }
}

// Moving a boundary between two adjacent texts must change the hash: the
// encoding carries the length of each text.
TEST(TagSetHash, KeepsAdjacentTextsApart) {
    TagSet first = samples::one_owner();
    first.tags = {{.name = "ab", .owner = "p"}};
    TagSet second;
    second.processes = {{.name = "bp", .uid = samples::UID_OTHER}};
    second.owners = {{.name = "bp", .process = "bp"}};
    second.tags = {{.name = "a", .owner = "bp"}};
    EXPECT_NE(hash_of(first), hash_of(second));
}

}  // namespace

namespace {

// Decision D-62: a RETAIN_NOW change is on disk within 1 s only with checks
// at most 400 ms apart.
TEST(TagSetValidate, RefusesASlowCheckWhenATagIsRetainNow) {
    TagSet tag_set = samples::one_owner();
    tag_set.tags = {{.name = "p.mode",
                     .owner = "p",
                     .type = TagType::INT64,
                     .retain = true}};
    tag_set.heartbeat_check_ms = MAX_RETAIN_NOW_CHECK_MS + 1;
    expect_valid(tag_set);  // no retain_now tag
    tag_set.tags.front().retain_now = true;
    expect_rejected(tag_set, StatusCode::INVALID_ARGUMENT,
                    "heartbeat_check_ms must be at most 400 in a tag set "
                    "with a retain_now tag (p.mode)");
    tag_set.heartbeat_check_ms = MAX_RETAIN_NOW_CHECK_MS;
    expect_valid(tag_set);
}

}  // namespace
