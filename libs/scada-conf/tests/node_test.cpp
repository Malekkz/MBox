// The node type: a Document is built only by a DocumentBuilder, and every
// accessor answers without throwing.

#include "scada/conf/node.h"

#include "scada/common/result.h"
#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace scada::conf {
namespace {

SourcePosition at(std::uint32_t line, std::uint32_t column,
                  FileIndex file = 0) {
    return SourcePosition{.file = file, .line = line, .column = column};
}

// { b: 1, a: [x, y], c: {} } from two files. Every step must succeed.
Document sample() {
    DocumentBuilder builder;
    bool good = builder.add_file("one.yaml").ok();
    good = good && builder.add_file("two.yaml").ok();
    good = good && builder.begin_map(at(1, 1)).ok();
    good = good && builder.key("b", ScalarStyle::PLAIN, at(1, 1)).ok();
    good =
        good &&
        builder.scalar("1", ScalarStyle::PLAIN, at(1, 4), ScalarType::INTEGER)
            .ok();
    good = good && builder.key("a", ScalarStyle::PLAIN, at(2, 1)).ok();
    good = good && builder.begin_sequence(at(2, 4)).ok();
    good = good && builder.scalar("x", ScalarStyle::PLAIN, at(2, 3)).ok();
    good = good &&
           builder.scalar("y", ScalarStyle::SINGLE_QUOTED, at(3, 4, 1)).ok();
    good = good && builder.end_sequence().ok();
    good = good && builder.key("c", ScalarStyle::DOUBLE_QUOTED, at(4, 1)).ok();
    good = good && builder.begin_map(at(4, 4)).ok();
    good = good && builder.end_map().ok();
    good = good && builder.end_map().ok();
    EXPECT_TRUE(good);
    Result<Document> built = builder.finish();
    if (!built.ok()) {
        ADD_FAILURE() << "the sample does not build";
        DocumentBuilder empty;
        static_cast<void>(empty.add_file("one.yaml"));
        static_cast<void>(empty.begin_map(at(1, 1)));
        static_cast<void>(empty.end_map());
        return std::move(empty.finish()).value();
    }
    return std::move(built).value();
}

TEST(Node, ABuiltDocumentKeepsOrderTextAndPositions) {
    const Document document = sample();
    const NodeId root = document.root();
    EXPECT_EQ(document.kind(root), NodeKind::MAP);
    ASSERT_EQ(document.entry_count(root), 3U);
    const auto entries = document.entries(root);
    EXPECT_EQ(document.text(entries[0].key), "b");
    EXPECT_EQ(document.text(entries[1].key), "a");
    EXPECT_EQ(document.text(entries[2].key), "c");
    EXPECT_EQ(document.resolved(entries[0].value), ScalarType::INTEGER);
    const NodeId list = entries[1].value;
    ASSERT_EQ(document.items(list).size(), 2U);
    EXPECT_EQ(document.entry_count(list), 2U);
    const NodeId y_item = document.items(list)[1];
    EXPECT_EQ(document.text(y_item), "y");
    EXPECT_EQ(document.style(y_item), ScalarStyle::SINGLE_QUOTED);
    EXPECT_EQ(document.location(y_item).file, "two.yaml");
    EXPECT_EQ(document.location(y_item).line, 3U);
    EXPECT_EQ(document.position(y_item).file, 1U);
    EXPECT_EQ(document.files().size(), 2U);
}

TEST(Node, AccessorsOfTheWrongKindAreEmpty) {
    const Document document = sample();
    const NodeId root = document.root();
    const NodeId scalar = document.entries(root)[0].value;
    EXPECT_TRUE(document.entries(scalar).empty());
    EXPECT_TRUE(document.items(scalar).empty());
    EXPECT_EQ(document.entry_count(scalar), 0U);
    EXPECT_TRUE(document.items(root).empty());
    EXPECT_TRUE(document.text(root).empty());
    EXPECT_FALSE(document.find(scalar, "b"));
    EXPECT_TRUE(document.entries(document.entries(root)[2].value).empty());
}

TEST(Node, FindLooksUpEveryKey) {
    const Document document = sample();
    const NodeId root = document.root();
    const auto entries = document.entries(root);
    EXPECT_EQ(document.find(root, "a"), entries[1].value);
    EXPECT_EQ(document.find(root, "b"), entries[0].value);
    EXPECT_EQ(document.find(root, "c"), entries[2].value);
    EXPECT_FALSE(document.find(root, ""));
    EXPECT_FALSE(document.find(root, "bb"));
    EXPECT_FALSE(document.find(root, "0"));
}

// A map of `count` keys `k<n>`, each with its key as its value, added in an
// order other than the keys' byte order.
Document big_map(std::size_t count) {
    constexpr std::size_t STRIDE = 7;
    DocumentBuilder builder;
    bool good = builder.add_file("f.yaml").ok();
    good = good && builder.begin_map(at(1, 1)).ok();
    for (std::size_t i = count; i > 0 && good; --i) {
        const std::string key = "k" + std::to_string(i * STRIDE % count);
        good = builder.key(key, ScalarStyle::PLAIN, at(1, 1)).ok() &&
               builder.scalar(key, ScalarStyle::PLAIN, at(1, 1)).ok();
    }
    good = good && builder.end_map().ok();
    EXPECT_TRUE(good);
    Result<Document> built = builder.finish();
    if (!built.ok()) {
        ADD_FAILURE() << "the map does not build";
        return sample();
    }
    return std::move(built).value();
}

TEST(Node, FindLooksUpEveryKeyOfALargeMap) {
    constexpr std::size_t KEYS = 1000;
    const Document document = big_map(KEYS);
    std::size_t found = 0;
    for (std::size_t i = 0; i < KEYS; ++i) {
        const std::string key = "k" + std::to_string(i);
        const std::optional<NodeId> value = document.find(document.root(), key);
        if (value.has_value() && document.text(*value) == key) {
            ++found;
        }
    }
    EXPECT_EQ(found, KEYS);
    EXPECT_FALSE(document.find(document.root(), "k1000"));
}

TEST(Node, ADuplicateKeyIsRefusedAndTheBuilderStaysFailed) {
    DocumentBuilder builder;
    ASSERT_TRUE(builder.add_file("f.yaml").ok());
    ASSERT_TRUE(builder.begin_map(at(1, 1)).ok());
    ASSERT_TRUE(builder.key("a", ScalarStyle::PLAIN, at(1, 1)).ok());
    ASSERT_TRUE(builder.scalar("1", ScalarStyle::PLAIN, at(1, 4)).ok());
    const Result<NodeId> again =
        builder.key("a", ScalarStyle::SINGLE_QUOTED, at(2, 1));
    ASSERT_FALSE(again.ok());
    EXPECT_EQ(again.status().code(), StatusCode::ALREADY_EXISTS);
    EXPECT_FALSE(builder.scalar("2", ScalarStyle::PLAIN, at(2, 6)).ok());
    EXPECT_EQ(builder.finish().status().code(), StatusCode::ALREADY_EXISTS);
}

TEST(Node, TheBuilderRefusesMisuse) {
    {
        DocumentBuilder builder;
        ASSERT_TRUE(builder.add_file("f.yaml").ok());
        ASSERT_TRUE(builder.begin_map(at(1, 1)).ok());
        // A value where a key is due.
        EXPECT_EQ(
            builder.scalar("x", ScalarStyle::PLAIN, at(1, 1)).status().code(),
            StatusCode::FAILED_PRECONDITION);
    }
    {
        DocumentBuilder builder;
        ASSERT_TRUE(builder.add_file("f.yaml").ok());
        ASSERT_TRUE(builder.begin_map(at(1, 1)).ok());
        ASSERT_TRUE(builder.key("a", ScalarStyle::PLAIN, at(1, 1)).ok());
        // A map ended while its last key waits for a value.
        EXPECT_EQ(builder.end_map().code(), StatusCode::FAILED_PRECONDITION);
    }
    {
        DocumentBuilder builder;
        ASSERT_TRUE(builder.add_file("f.yaml").ok());
        ASSERT_TRUE(builder.begin_sequence(at(1, 1)).ok());
        EXPECT_EQ(builder.end_map().code(), StatusCode::FAILED_PRECONDITION);
    }
    {
        DocumentBuilder builder;
        ASSERT_TRUE(builder.add_file("f.yaml").ok());
        // A file index that was not added.
        EXPECT_EQ(builder.scalar("x", ScalarStyle::PLAIN, at(1, 1, 1))
                      .status()
                      .code(),
                  StatusCode::INVALID_ARGUMENT);
    }
    {
        DocumentBuilder builder;
        ASSERT_TRUE(builder.add_file("f.yaml").ok());
        ASSERT_TRUE(builder.scalar("x", ScalarStyle::PLAIN, at(1, 1)).ok());
        // A second root.
        EXPECT_FALSE(builder.scalar("y", ScalarStyle::PLAIN, at(2, 1)).ok());
    }
    {
        DocumentBuilder builder;
        ASSERT_TRUE(builder.add_file("f.yaml").ok());
        ASSERT_TRUE(builder.begin_sequence(at(1, 1)).ok());
        // Not complete.
        EXPECT_EQ(builder.finish().status().code(),
                  StatusCode::FAILED_PRECONDITION);
    }
    {
        DocumentBuilder builder;
        EXPECT_EQ(builder.finish().status().code(),
                  StatusCode::FAILED_PRECONDITION);
    }
}

TEST(Node, TheBuilderCanBeUsedAgainAfterFinish) {
    DocumentBuilder builder;
    ASSERT_TRUE(builder.add_file("f.yaml").ok());
    ASSERT_TRUE(builder.scalar("x", ScalarStyle::PLAIN, at(1, 1)).ok());
    ASSERT_TRUE(builder.finish().ok());
    EXPECT_EQ(builder.size(), 0U);
    ASSERT_TRUE(builder.add_file("g.yaml").ok());
    ASSERT_TRUE(builder.scalar("y", ScalarStyle::PLAIN, at(1, 1)).ok());
    const Result<Document> second = builder.finish();
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().files().size(), 1U);
    EXPECT_EQ(second.value().text(second.value().root()), "y");
}

}  // namespace
}  // namespace scada::conf
