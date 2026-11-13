#ifndef SCADA_TAG_NAME_HASH_H
#define SCADA_TAG_NAME_HASH_H

// The hash of a tag name in the tag directory
// (docs/contracts/tag-database.md, section 5, `DirectoryEntry.name_hash`).

#include <cstdint>
#include <string_view>

namespace scada::tag {

inline constexpr std::uint64_t FNV1A_64_OFFSET_BASIS = 0xCBF2'9CE4'8422'2325U;
inline constexpr std::uint64_t FNV1A_64_PRIME = 0x0000'0100'0000'01B3U;

// Returns the 64-bit FNV-1a hash of the bytes of `name`. It accepts any
// bytes; whether `name` is a valid tag name is checked elsewhere.
[[nodiscard]] constexpr std::uint64_t fnv1a_64(std::string_view name) noexcept {
    std::uint64_t hash = FNV1A_64_OFFSET_BASIS;
    for (const char character : name) {
        hash ^= static_cast<std::uint8_t>(character);
        // Wraps modulo 2^64; the algorithm is defined that way.
        hash *= FNV1A_64_PRIME;
    }
    return hash;
}

}  // namespace scada::tag

#endif  // SCADA_TAG_NAME_HASH_H
