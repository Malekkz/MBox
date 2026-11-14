// Unit tests for the temporary tag-set file format: what a document turns
// into, the defaults, and that malformed input is rejected with the place
// of the error.

#include "scada/tag/tag_set_json.h"

#include "tag_set_samples.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/header_builder.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace scada::tag;
using scada::Result;
using scada::StatusCode;
using std::string_view_literals::operator""sv;

// samples::sample() as a document, with the entries in another order.
constexpr std::string_view SAMPLE_DOCUMENT = R"json({
  "format": 1,
  "heartbeat_check_ms": 250,
  "processes": [
    {"name": "io", "uid": 991, "heartbeat_timeout_ms": 500, "realtime": true},
    {"name": "control", "uid": 992, "realtime": true},
    {"name": "journal", "uid": 993}
  ],
  "owners": [
    {"name": "io", "process": "io", "change_ring_capacity": 1024},
    {"name": "control.fast", "process": "control"},
    {"name": "control.slow", "process": "control"}
  ],
  "queues": [
    {"name": "events.control.fast", "producer": "control",
     "consumer": "journal", "capacity": 4096},
    {"name": "cmd.io.control.fast", "producer": "control", "consumer": "io"},
    {"name": "result.io.control.fast", "producer": "io", "consumer": "control"}
  ],
  "tags": [
    {"name": "plant.pump1.running", "owner": "io", "type": "bool"},
    {"name": "plant.pump1.speed", "owner": "io", "type": "float64",
     "writable_by_command": true},
    {"name": "plant.pump1.mode", "owner": "control.slow", "type": "int64",
     "retain": true, "retain_now": true},
    {"name": "plant.pump1.starts", "owner": "control.fast", "type": "uint64",
     "retain": true},
    {"name": "plant.pump1.label", "owner": "control.slow", "type": "string"}
  ]
}
)json";

TagSet parse(std::string_view text) {
    std::string detail = "untouched";
    Result<TagSet> tag_set = parse_tag_set_json(text, detail);
    EXPECT_TRUE(tag_set.ok()) << detail;
    EXPECT_EQ(detail, "untouched");
    return tag_set.ok() ? std::move(tag_set).value() : TagSet{};
}

// Expects that `text` is rejected with `code` and exactly `detail`.
void expect_rejected(std::string_view text, StatusCode code,
                     std::string_view expected_detail) {
    std::string detail;
    const Result<TagSet> tag_set = parse_tag_set_json(text, detail);
    EXPECT_EQ(tag_set.status().code(), code) << text;
    EXPECT_EQ(detail, expected_detail) << text;
}

void expect_corrupt(std::string_view text, std::string_view expected_detail) {
    expect_rejected(text, StatusCode::DATA_CORRUPT, expected_detail);
}

TEST(TagSetJson, ReadsEveryFieldOfTheSample) {
    const TagSet tag_set = parse(SAMPLE_DOCUMENT);
    EXPECT_EQ(tag_set.heartbeat_check_ms, 250U);

    ASSERT_EQ(tag_set.processes.size(), 3U);
    EXPECT_EQ(tag_set.processes[0].name, "io");
    EXPECT_EQ(tag_set.processes[0].uid, 991U);
    EXPECT_EQ(tag_set.processes[0].heartbeat_timeout_ms, 500U);
    EXPECT_TRUE(tag_set.processes[0].realtime);
    EXPECT_EQ(tag_set.processes[2].name, "journal");
    EXPECT_FALSE(tag_set.processes[2].realtime);

    ASSERT_EQ(tag_set.owners.size(), 3U);
    EXPECT_EQ(tag_set.owners[0].name, "io");
    EXPECT_EQ(tag_set.owners[0].process, "io");
    EXPECT_EQ(tag_set.owners[0].change_ring_capacity, 1024U);
    EXPECT_EQ(tag_set.owners[1].name, "control.fast");
    EXPECT_EQ(tag_set.owners[1].process, "control");

    ASSERT_EQ(tag_set.queues.size(), 3U);
    EXPECT_EQ(tag_set.queues[0].name, "events.control.fast");
    EXPECT_EQ(tag_set.queues[0].producer, "control");
    EXPECT_EQ(tag_set.queues[0].consumer, "journal");
    EXPECT_EQ(tag_set.queues[0].capacity, 4096U);

    ASSERT_EQ(tag_set.tags.size(), 5U);
    EXPECT_EQ(tag_set.tags[0].name, "plant.pump1.running");
    EXPECT_EQ(tag_set.tags[0].owner, "io");
    EXPECT_EQ(tag_set.tags[0].type, TagType::BOOL);
    EXPECT_EQ(tag_set.tags[1].type, TagType::FLOAT64);
    EXPECT_TRUE(tag_set.tags[1].writable_by_command);
    EXPECT_FALSE(tag_set.tags[1].retain);
    EXPECT_EQ(tag_set.tags[2].type, TagType::INT64);
    EXPECT_TRUE(tag_set.tags[2].retain);
    EXPECT_TRUE(tag_set.tags[2].retain_now);
    EXPECT_EQ(tag_set.tags[3].type, TagType::UINT64);
    EXPECT_EQ(tag_set.tags[4].type, TagType::STRING);
}

TEST(TagSetJson, FillsInTheDefaults) {
    const TagSet tag_set = parse(SAMPLE_DOCUMENT);
    // Real-time and general heartbeat timeouts (contract, section 10.1).
    EXPECT_EQ(tag_set.processes[1].heartbeat_timeout_ms,
              DEFAULT_REALTIME_HEARTBEAT_TIMEOUT_MS);
    EXPECT_EQ(tag_set.processes[2].heartbeat_timeout_ms,
              DEFAULT_HEARTBEAT_TIMEOUT_MS);
    EXPECT_EQ(tag_set.owners[1].change_ring_capacity,
              DEFAULT_CHANGE_RING_CAPACITY);
    EXPECT_EQ(tag_set.queues[1].capacity, DEFAULT_QUEUE_CAPACITY);
    EXPECT_FALSE(tag_set.tags[0].retain);
    EXPECT_FALSE(tag_set.tags[0].writable_by_command);
    EXPECT_FALSE(tag_set.tags[0].retain_now);

    const TagSet minimal = parse(R"({"format":1})");
    EXPECT_EQ(minimal.heartbeat_check_ms, DEFAULT_HEARTBEAT_CHECK_MS);
    EXPECT_TRUE(minimal.processes.empty());
    EXPECT_TRUE(minimal.tags.empty());

    // An explicit timeout of a real-time process is kept, whatever the
    // order of the keys.
    const TagSet explicit_timeout = parse(
        R"({"format":1,"processes":[)"
        R"({"heartbeat_timeout_ms":900,"realtime":true,"uid":1,"name":"p"}]})");
    EXPECT_EQ(explicit_timeout.processes.at(0).heartbeat_timeout_ms, 900U);
}

// The document and the tag set built in code describe the same generation.
TEST(TagSetJson, GivesTheSameHeaderAsTheSameTagSetBuiltInCode) {
    std::string detail;
    const Result<HeaderImage> from_document =
        build_header(parse(SAMPLE_DOCUMENT), {}, detail);
    ASSERT_TRUE(from_document.ok()) << detail;
    const Result<HeaderImage> from_code =
        build_header(samples::sample(), {}, detail);
    ASSERT_TRUE(from_code.ok()) << detail;
    EXPECT_EQ(from_document.value().config_hash, from_code.value().config_hash);
    EXPECT_TRUE(from_document.value().bytes == from_code.value().bytes);
}

TEST(TagSetJson, AcceptsWhitespaceBetweenAllTokens) {
    const TagSet tag_set = parse(
        " \t\r\n{ \"format\" : 1 , \"processes\" : [ { \"name\" : \"p\" ,\n"
        "\"uid\" : 0 } ] , \"tags\" : [ ] }\n\n");
    ASSERT_EQ(tag_set.processes.size(), 1U);
    EXPECT_EQ(tag_set.processes[0].uid, 0U);
}

TEST(TagSetJson, RejectsBrokenStructureWithLineAndColumn) {
    expect_corrupt("", "line 1, column 1: expected '{'");
    expect_corrupt("   ", "line 1, column 4: expected '{'");
    expect_corrupt("[]", "line 1, column 1: expected '{'");
    expect_corrupt("{", "line 1, column 2: expected a string");
    expect_corrupt(R"({"format")",
                   "line 1, column 10: expected ':' after a key");
    expect_corrupt(R"({"format":1)", "line 1, column 12: expected ',' or '}'");
    expect_corrupt(R"({"format":1,})", "line 1, column 13: expected a string");
    expect_corrupt(R"({"format":1} x)",
                   "line 1, column 14: unexpected text after the document");
    expect_corrupt(R"({"format":1}{})",
                   "line 1, column 13: unexpected text after the document");
    expect_corrupt("{\"format\":1,\n\"tags\":{}}",
                   "line 2, column 8: expected '['");
    expect_corrupt("{\"format\":1,\n \"tags\":[\n  {\"name\":\"a\"",
                   "line 3, column 14: expected ',' or '}'");
    expect_corrupt(
        R"({"format":1,"tags":[{"name":"a","owner":"o","type":"bool"})",
        "line 1, column 59: expected ',' or ']'");
    expect_corrupt(R"({"format":1,"tags":[1]})",
                   "line 1, column 21: expected '{'");
    expect_corrupt(R"({"format":1,"tags":[,]})",
                   "line 1, column 21: expected '{'");
}

TEST(TagSetJson, RejectsUnknownDuplicateAndMissingKeys) {
    expect_corrupt(R"({"format":1,"colour":1})",
                   "line 1, column 13: unknown key");
    expect_corrupt(R"({"format":1,"Format":1})",
                   "line 1, column 13: unknown key");
    expect_corrupt(R"({"format":1,"format":1})",
                   "line 1, column 13: key appears twice");
    expect_corrupt(R"({"format":1,"tags":[],"tags":[]})",
                   "line 1, column 23: key appears twice");
    expect_corrupt(R"({"format":1,"tags":[{"name":"a","name":"b"}]})",
                   "line 1, column 33: key appears twice");
    expect_corrupt("{}", "line 1, column 3: a required key is missing: format");
    expect_corrupt(R"({"format":1,"processes":[{"name":"p"}]})",
                   "line 1, column 38: a required key is missing: uid");
    expect_corrupt(R"({"format":1,"owners":[{"name":"p"}]})",
                   "line 1, column 35: a required key is missing: process");
    expect_corrupt(
        R"({"format":1,"queues":[{"name":"events.p","producer":"p"}]})",
        "line 1, column 57: a required key is missing: consumer");
    expect_corrupt(R"({"format":1,"tags":[{"name":"a","owner":"o"}]})",
                   "line 1, column 45: a required key is missing: type");
    expect_corrupt(R"({"format":1,"tags":[{}]})",
                   "line 1, column 23: a required key is missing: name");
}

TEST(TagSetJson, RejectsStringsItDoesNotSupport) {
    expect_corrupt(R"({"format":1,"processes":[{"name":"p\n","uid":1}]})",
                   "line 1, column 36: escape sequences are not supported");
    expect_corrupt("{\"format\":1,\"processes\":[{\"name\":\"p\nq\"}]}",
                   "line 1, column 36: a string holds printable ASCII only");
    expect_corrupt("{\"format\":1,\"processes\":[{\"name\":\"\xC3\xA9\"}]}",
                   "line 1, column 35: a string holds printable ASCII only");
    // A NUL inside a key.
    expect_corrupt("{\"format\":1,\"x\0\":1}"sv,
                   "line 1, column 15: a string holds printable ASCII only");
    expect_corrupt(R"({"format":1,"processes":[{"name":"p)",
                   "line 1, column 35: string without closing quote");
    expect_corrupt(R"({"format":1,"processes":[{"name":p}]})",
                   "line 1, column 34: expected a string");

    // 127 bytes is the longest string; a tag name can be that long.
    const std::string longest(MAX_TAG_NAME_LENGTH, 'a');
    const std::string prefix =
        R"({"format":1,"tags":[{"type":"bool","owner":"o","name":")";
    const TagSet tag_set = parse(prefix + longest + R"("}]})");
    EXPECT_EQ(tag_set.tags.at(0).name, longest);
    expect_rejected(prefix + longest + R"(a"}]})", StatusCode::OUT_OF_RANGE,
                    "line 1, column " + std::to_string(prefix.size() + 1) +
                        ": string too long");
}

TEST(TagSetJson, RejectsNumbersThatAreNotSmallWholeNumbers) {
    const auto with_value = [](std::string_view value) {
        return R"({"format":1,"heartbeat_check_ms":)" + std::string(value) +
               "}";
    };
    EXPECT_EQ(parse(with_value("0")).heartbeat_check_ms, 0U);
    EXPECT_EQ(parse(with_value("4294967295")).heartbeat_check_ms,
              4'294'967'295U);
    expect_rejected(with_value("4294967296"), StatusCode::OUT_OF_RANGE,
                    "line 1, column 34: number too large");
    expect_rejected(with_value("99999999999999999999999"),
                    StatusCode::OUT_OF_RANGE,
                    "line 1, column 34: number too large");
    expect_corrupt(with_value("-1"),
                   "line 1, column 34: expected a whole number that is not "
                   "negative");
    expect_corrupt(with_value("007"),
                   "line 1, column 34: number with a leading zero");
    expect_corrupt(with_value("1.5"),
                   "line 1, column 34: expected a whole number");
    expect_corrupt(with_value("1e3"),
                   "line 1, column 34: expected a whole number");
    expect_corrupt(with_value("1E3"),
                   "line 1, column 34: expected a whole number");
    expect_corrupt(with_value("\"250\""),
                   "line 1, column 34: expected a whole number that is not "
                   "negative");
    expect_corrupt(with_value("12x"), "line 1, column 36: expected ',' or '}'");
    expect_corrupt(with_value("null"),
                   "line 1, column 34: expected a whole number that is not "
                   "negative");
}

TEST(TagSetJson, RejectsOtherWrongValues) {
    expect_corrupt(
        R"({"format":1,"processes":[{"name":"p","uid":1,"realtime":1}]})",
        "line 1, column 57: expected true or false");
    expect_corrupt(
        R"({"format":1,"processes":[{"name":"p","uid":1,"realtime":True}]})",
        "line 1, column 57: expected true or false");
    expect_corrupt(
        R"({"format":1,"processes":[{"name":"p","uid":1,"realtime":truely}]})",
        "line 1, column 61: expected ',' or '}'");
    expect_corrupt(
        R"({"format":1,"tags":[{"name":"a","owner":"o","type":"float32"}]})",
        "line 1, column 52: unknown tag type");
    expect_corrupt(
        R"({"format":1,"tags":[{"name":"a","owner":"o","type":"BOOL"}]})",
        "line 1, column 52: unknown tag type");
    expect_rejected(R"({"format":2})", StatusCode::UNSUPPORTED,
                    "line 1, column 11: unsupported format version");
    expect_rejected(R"({"format":0})", StatusCode::UNSUPPORTED,
                    "line 1, column 11: unsupported format version");
}

// A list longer than the limit of the contract is refused while it is read,
// before the rest of it is stored.
TEST(TagSetJson, RejectsAListBeyondTheLimitOfTheContract) {
    std::string document = R"({"format":1,"processes":[)";
    for (std::size_t i = 0; i < MAX_PROCESSES; ++i) {
        document += R"({"name":"p","uid":1},)";
    }
    const std::size_t column = document.size() + 1;
    EXPECT_EQ(
        parse(document.substr(0, document.size() - 1) + "]}").processes.size(),
        MAX_PROCESSES);
    document += R"({"name":"p","uid":1}]})";
    expect_rejected(document, StatusCode::OUT_OF_RANGE,
                    "line 1, column " + std::to_string(column) +
                        ": too many entries in a list");
}

TEST(TagSetJson, RejectsADocumentBeyondTheSizeLimit) {
    // Only the size is looked at, so the content can be anything.
    const std::string document(MAX_TAG_SET_JSON_BYTES + 1, ' ');
    expect_rejected(document, StatusCode::OUT_OF_RANGE,
                    "the document is larger than 268435456 bytes");
}

// The parser checks the shape only; the contract rules are `validate`'s.
TEST(TagSetJson, LeavesTheContractRulesToValidate) {
    const TagSet tag_set = parse(
        R"({"format":1,"tags":[{"name":"Not A Name","owner":"nobody","type":"bool"}]})");
    std::string detail;
    EXPECT_EQ(validate(tag_set, detail).code(), StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(detail,
              "tag at position 0 (from 0): the name must be 1 to 127 "
              "bytes of [a-z0-9_.-]");
}

}  // namespace
