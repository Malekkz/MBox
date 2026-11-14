#ifndef SCADA_TAG_SHA256_H
#define SCADA_TAG_SHA256_H

// SHA-256 (FIPS 180-4), for `GlobalHeader.config_hash`. It identifies a
// tag-set definition; it is not used to protect a secret or to authenticate
// anything.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

inline constexpr std::size_t SHA256_DIGEST_BYTES = 32;
inline constexpr std::size_t SHA256_BLOCK_BYTES = 64;
inline constexpr std::size_t SHA256_STATE_WORDS = 8;
using Sha256Digest = std::array<std::uint8_t, SHA256_DIGEST_BYTES>;

// Computes one digest: any number of `update` calls, then one `finish`.
class Sha256 {
public:
    // Appends `data` to the message.
    void update(std::span<const std::uint8_t> data) noexcept;

    // Returns the digest of everything appended. The object must not be used
    // again afterwards.
    [[nodiscard]] Sha256Digest finish() noexcept;

private:
    // Mixes the full `block_` into `state_`.
    void compress() noexcept;

    // The initial hash value: the first 32 bits of the fractional parts of
    // the square roots of the first eight primes.
    std::array<std::uint32_t, SHA256_STATE_WORDS> state_{
        0x6A09'E667U, 0xBB67'AE85U, 0x3C6E'F372U, 0xA54F'F53AU,
        0x510E'527FU, 0x9B05'688CU, 0x1F83'D9ABU, 0x5BE0'CD19U};
    std::array<std::uint8_t, SHA256_BLOCK_BYTES> block_{};
    std::size_t block_used_ = 0;
    std::uint64_t message_bytes_ = 0;
};

// Returns the SHA-256 digest of `data`.
[[nodiscard]] Sha256Digest sha256(std::span<const std::uint8_t> data) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_SHA256_H
