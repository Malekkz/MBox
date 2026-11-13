#ifndef SCADA_TOY_TOY_PARSER_H
#define SCADA_TOY_TOY_PARSER_H

// A toy message parser with a PLANTED BUG, used only as the sample for the
// fuzz build (issue P0-03). It must never be linked into product code.
//
// Message format, all sizes in bytes:
//   magic 'T' (1), version 0x01 (1), then records until the end of input.
//   Record: type (1), payload size (1), payload.
//     type 1  value  payload is exactly 2: unsigned, big-endian
//     type 2  name   payload is at most MAX_NAME_SIZE
//     type 3  label  payload is at most LABEL_CAPACITY
// Limits: at most MAX_MESSAGE_SIZE of input and MAX_VALUES value records.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scada::toy {

inline constexpr std::size_t MAX_MESSAGE_SIZE = 256;
inline constexpr std::size_t MAX_VALUES = 32;
inline constexpr std::size_t MAX_NAME_SIZE = 16;
inline constexpr std::size_t LABEL_CAPACITY = 8;

struct Message {
    std::vector<std::uint16_t> values;
    std::string name;
    std::string label;
};

// Parses one message. `input` is untrusted. Returns nothing if the input is
// not a well-formed message within the limits above; no partial result is
// returned.
[[nodiscard]] std::optional<Message> parse(std::span<const std::byte> input);

}  // namespace scada::toy

#endif  // SCADA_TOY_TOY_PARSER_H
