#ifndef SCADA_TAG_CRC32_H
#define SCADA_TAG_CRC32_H

// CRC-32 of the retain file (docs/contracts/tag-database.md, section 12):
// the IEEE 802.3 polynomial, reflected (0xEDB88320), initial value and
// final XOR 0xFFFFFFFF, as zlib computes it. It detects damage; it
// protects nothing against a writer that means harm. Private to
// libscada-tag.

#include <cstddef>
#include <cstdint>
#include <span>

namespace scada::tag {

// The CRC-32 of `bytes`. "123456789" gives 0xCBF43926.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_CRC32_H
