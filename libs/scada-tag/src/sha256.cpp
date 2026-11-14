#include "sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

namespace {

constexpr std::size_t ROUNDS = 64;
constexpr std::size_t WORD_BYTES = 4;
constexpr std::size_t BLOCK_WORDS = SHA256_BLOCK_BYTES / WORD_BYTES;
constexpr std::size_t LENGTH_BYTES = 8;
constexpr unsigned BITS_PER_BYTE = 8;
constexpr std::uint8_t PADDING_START = 0x80;

// The round constants: the first 32 bits of the fractional parts of the cube
// roots of the first 64 primes.
constexpr std::array<std::uint32_t, ROUNDS> ROUND_CONSTANTS{
    0x428A'2F98U, 0x7137'4491U, 0xB5C0'FBCFU, 0xE9B5'DBA5U, 0x3956'C25BU,
    0x59F1'11F1U, 0x923F'82A4U, 0xAB1C'5ED5U, 0xD807'AA98U, 0x1283'5B01U,
    0x2431'85BEU, 0x550C'7DC3U, 0x72BE'5D74U, 0x80DE'B1FEU, 0x9BDC'06A7U,
    0xC19B'F174U, 0xE49B'69C1U, 0xEFBE'4786U, 0x0FC1'9DC6U, 0x240C'A1CCU,
    0x2DE9'2C6FU, 0x4A74'84AAU, 0x5CB0'A9DCU, 0x76F9'88DAU, 0x983E'5152U,
    0xA831'C66DU, 0xB003'27C8U, 0xBF59'7FC7U, 0xC6E0'0BF3U, 0xD5A7'9147U,
    0x06CA'6351U, 0x1429'2967U, 0x27B7'0A85U, 0x2E1B'2138U, 0x4D2C'6DFCU,
    0x5338'0D13U, 0x650A'7354U, 0x766A'0ABBU, 0x81C2'C92EU, 0x9272'2C85U,
    0xA2BF'E8A1U, 0xA81A'664BU, 0xC24B'8B70U, 0xC76C'51A3U, 0xD192'E819U,
    0xD699'0624U, 0xF40E'3585U, 0x106A'A070U, 0x19A4'C116U, 0x1E37'6C08U,
    0x2748'774CU, 0x34B0'BCB5U, 0x391C'0CB3U, 0x4ED8'AA4AU, 0x5B9C'CA4FU,
    0x682E'6FF3U, 0x748F'82EEU, 0x78A5'636FU, 0x84C8'7814U, 0x8CC7'0208U,
    0x90BE'FFFAU, 0xA450'6CEBU, 0xBEF9'A3F7U, 0xC671'78F2U};

// The rotation and shift amounts of the four mixing functions of the
// standard (section 4.1.2).
struct Mix {
    int first = 0;
    int second = 0;
    int third = 0;
};
constexpr Mix BIG_SIGMA_0{2, 13, 22};
constexpr Mix BIG_SIGMA_1{6, 11, 25};
constexpr Mix SMALL_SIGMA_0{7, 18, 3};    // the third is a shift
constexpr Mix SMALL_SIGMA_1{17, 19, 10};  // the third is a shift

// How far back the message schedule reaches for each of its four terms.
constexpr std::size_t SCHEDULE_BACK_SIGMA_1 = 2;
constexpr std::size_t SCHEDULE_BACK_PLAIN_NEAR = 7;
constexpr std::size_t SCHEDULE_BACK_SIGMA_0 = 15;
constexpr std::size_t SCHEDULE_BACK_PLAIN_FAR = 16;

constexpr std::uint32_t big_sigma(std::uint32_t word, const Mix& mix) noexcept {
    return std::rotr(word, mix.first) ^ std::rotr(word, mix.second) ^
           std::rotr(word, mix.third);
}

constexpr std::uint32_t small_sigma(std::uint32_t word,
                                    const Mix& mix) noexcept {
    return std::rotr(word, mix.first) ^ std::rotr(word, mix.second) ^
           (word >> static_cast<unsigned>(mix.third));
}

// Reads the big-endian 32-bit word at `bytes[0..3]`.
constexpr std::uint32_t load_word(
    std::span<const std::uint8_t, WORD_BYTES> bytes) noexcept {
    std::uint32_t word = 0;
    for (const std::uint8_t byte : bytes) {
        word = (word << BITS_PER_BYTE) | byte;
    }
    return word;
}

// The working variables a to h of the standard, in the order of the hash
// state.
struct Working {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t d = 0;
    std::uint32_t e = 0;
    std::uint32_t f = 0;
    std::uint32_t g = 0;
    std::uint32_t h = 0;
};
using State = std::array<std::uint32_t, SHA256_STATE_WORDS>;
static_assert(sizeof(Working) == sizeof(State));

// One round. All additions wrap modulo 2^32; the algorithm is defined that
// way.
constexpr Working round(const Working& vars, std::uint32_t constant,
                        std::uint32_t schedule_word) noexcept {
    const std::uint32_t choose = (vars.e & vars.f) ^ (~vars.e & vars.g);
    const std::uint32_t majority =
        (vars.a & vars.b) ^ (vars.a & vars.c) ^ (vars.b & vars.c);
    const std::uint32_t first = vars.h + big_sigma(vars.e, BIG_SIGMA_1) +
                                choose + constant + schedule_word;
    const std::uint32_t second = big_sigma(vars.a, BIG_SIGMA_0) + majority;
    return Working{first + second, vars.a, vars.b, vars.c,
                   vars.d + first, vars.e, vars.f, vars.g};
}

}  // namespace

void Sha256::compress() noexcept {
    std::array<std::uint32_t, ROUNDS> schedule{};
    const std::span<const std::uint8_t, SHA256_BLOCK_BYTES> block(block_);
    for (std::size_t i = 0; i < BLOCK_WORDS; ++i) {
        schedule.at(i) =
            load_word(block.subspan(i * WORD_BYTES).first<WORD_BYTES>());
    }
    for (std::size_t i = BLOCK_WORDS; i < ROUNDS; ++i) {
        schedule.at(i) =
            small_sigma(schedule.at(i - SCHEDULE_BACK_SIGMA_1), SMALL_SIGMA_1) +
            schedule.at(i - SCHEDULE_BACK_PLAIN_NEAR) +
            small_sigma(schedule.at(i - SCHEDULE_BACK_SIGMA_0), SMALL_SIGMA_0) +
            schedule.at(i - SCHEDULE_BACK_PLAIN_FAR);
    }

    auto working = std::bit_cast<Working>(state_);
    for (std::size_t i = 0; i < ROUNDS; ++i) {
        working = round(working, ROUND_CONSTANTS.at(i), schedule.at(i));
    }
    const auto mixed = std::bit_cast<State>(working);
    for (std::size_t i = 0; i < state_.size(); ++i) {
        state_.at(i) += mixed.at(i);
    }
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
    message_bytes_ += data.size();
    while (!data.empty()) {
        const std::size_t take =
            std::min(data.size(), SHA256_BLOCK_BYTES - block_used_);
        std::ranges::copy(data.first(take),
                          std::span(block_).subspan(block_used_).begin());
        block_used_ += take;
        data = data.subspan(take);
        if (block_used_ == SHA256_BLOCK_BYTES) {
            compress();
            block_used_ = 0;
        }
    }
}

Sha256Digest Sha256::finish() noexcept {
    // The length in bits, wrapping modulo 2^64 as the standard defines.
    const std::uint64_t message_bits = message_bytes_ * BITS_PER_BYTE;

    // Padding: one 0x80 byte, zeros up to 8 bytes before the end of a block,
    // then the length as a big-endian 64-bit number.
    block_.at(block_used_) = PADDING_START;
    ++block_used_;
    if (block_used_ > SHA256_BLOCK_BYTES - LENGTH_BYTES) {
        std::ranges::fill(std::span(block_).subspan(block_used_),
                          std::uint8_t{0});
        compress();
        block_used_ = 0;
    }
    std::ranges::fill(std::span(block_).subspan(block_used_), std::uint8_t{0});
    for (std::size_t i = 0; i < LENGTH_BYTES; ++i) {
        const auto shift =
            static_cast<unsigned>((LENGTH_BYTES - 1 - i) * BITS_PER_BYTE);
        // Truncation to the low byte is the intent.
        block_.at(SHA256_BLOCK_BYTES - LENGTH_BYTES + i) =
            static_cast<std::uint8_t>(message_bits >> shift);
    }
    compress();

    Sha256Digest digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        for (std::size_t j = 0; j < WORD_BYTES; ++j) {
            const auto shift =
                static_cast<unsigned>((WORD_BYTES - 1 - j) * BITS_PER_BYTE);
            // Truncation to the low byte is the intent.
            digest.at((i * WORD_BYTES) + j) =
                static_cast<std::uint8_t>(state_.at(i) >> shift);
        }
    }
    return digest;
}

Sha256Digest sha256(std::span<const std::uint8_t> data) noexcept {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}

}  // namespace scada::tag
