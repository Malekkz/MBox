// Unit tests for SHA-256, against the test vectors of the standard
// (FIPS 180-4 and the NIST example values).

#include "sha256.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scada::tag;

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (const char character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    return bytes;
}

std::string hex(const Sha256Digest& digest) {
    constexpr std::string_view DIGITS = "0123456789abcdef";
    constexpr unsigned NIBBLE_BITS = 4;
    constexpr unsigned NIBBLE_MASK = 0xF;
    std::string text;
    for (const std::uint8_t byte : digest) {
        text.push_back(DIGITS.at(byte >> NIBBLE_BITS));
        text.push_back(DIGITS.at(byte & NIBBLE_MASK));
    }
    return text;
}

TEST(Sha256, MatchesThePublishedVectors) {
    EXPECT_EQ(
        hex(sha256(bytes_of(""))),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        hex(sha256(bytes_of("abc"))),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the padding does not fit the first block.
    EXPECT_EQ(
        hex(sha256(bytes_of(
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MatchesTheVectorOfOneMillionLetters) {
    constexpr std::size_t LETTERS = 1'000'000;
    const std::vector<std::uint8_t> message(LETTERS, std::uint8_t{'a'});
    EXPECT_EQ(
        hex(sha256(message)),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// The digest does not depend on how the message is cut into updates, for
// every length around the block and padding boundaries.
TEST(Sha256, IsTheSameForAnySplitOfTheMessage) {
    constexpr std::size_t MAX_LENGTH = 200;
    constexpr std::size_t CHUNK = 7;
    std::vector<std::uint8_t> message;
    for (std::size_t length = 0; length <= MAX_LENGTH; ++length) {
        const Sha256Digest whole = sha256(message);

        Sha256 in_chunks;
        const std::span<const std::uint8_t> all(message);
        for (std::size_t done = 0; done < all.size(); done += CHUNK) {
            in_chunks.update(
                all.subspan(done, std::min(CHUNK, all.size() - done)));
        }
        EXPECT_EQ(hex(in_chunks.finish()), hex(whole)) << "length " << length;

        Sha256 byte_by_byte;
        for (std::size_t i = 0; i < all.size(); ++i) {
            byte_by_byte.update(all.subspan(i, 1));
        }
        EXPECT_EQ(hex(byte_by_byte.finish()), hex(whole))
            << "length " << length;

        // Truncation to the low byte is the intent.
        message.push_back(static_cast<std::uint8_t>(length));
    }
}

}  // namespace
