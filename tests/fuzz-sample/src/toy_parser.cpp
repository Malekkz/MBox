#include "scada/toy/toy_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace scada::toy {

namespace {

constexpr std::byte MAGIC{'T'};
constexpr std::byte VERSION{0x01};
constexpr std::uint8_t RECORD_VALUE = 1;
constexpr std::uint8_t RECORD_NAME = 2;
constexpr std::uint8_t RECORD_LABEL = 3;
constexpr unsigned BITS_PER_BYTE = 8;

// Reads through the input and checks every read against what is left.
class Cursor {
public:
    explicit Cursor(std::span<const std::byte> input) : rest_(input) {}

    [[nodiscard]] bool at_end() const { return rest_.empty(); }

    [[nodiscard]] std::optional<std::uint8_t> read_u8() {
        if (rest_.empty()) {
            return std::nullopt;
        }
        const auto value = std::to_integer<std::uint8_t>(rest_.front());
        rest_ = rest_.subspan(1);
        return value;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> read_bytes(
        std::size_t count) {
        if (count > rest_.size()) {
            return std::nullopt;
        }
        const std::span<const std::byte> bytes = rest_.first(count);
        rest_ = rest_.subspan(count);
        return bytes;
    }

private:
    std::span<const std::byte> rest_;
};

std::string to_text(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    std::transform(
        bytes.begin(), bytes.end(), text.begin(),
        [](std::byte value) { return std::to_integer<char>(value); });
    return text;
}

// PLANTED BUG, deliberate: the size is checked against the limit for names
// (16) instead of the capacity of the label buffer (8), so a label of 9 to 16
// bytes is written past the end of a heap buffer. The fuzz build must find
// this; do not fix it.
std::optional<std::string> copy_label(std::span<const std::byte> payload) {
    if (payload.size() > MAX_NAME_SIZE) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): the planted bug needs a raw heap buffer
    const auto buffer = std::make_unique<std::byte[]>(LABEL_CAPACITY);
    std::copy(payload.begin(), payload.end(), buffer.get());
    return to_text(std::span<const std::byte>(buffer.get(), payload.size()));
}

}  // namespace

std::optional<Message> parse(std::span<const std::byte> input) {
    if (input.size() > MAX_MESSAGE_SIZE) {
        return std::nullopt;
    }
    Cursor cursor(input);
    if (cursor.read_u8() != std::to_integer<std::uint8_t>(MAGIC) ||
        cursor.read_u8() != std::to_integer<std::uint8_t>(VERSION)) {
        return std::nullopt;
    }

    Message message;
    while (!cursor.at_end()) {
        const std::optional<std::uint8_t> type = cursor.read_u8();
        const std::optional<std::uint8_t> size = cursor.read_u8();
        if (!type || !size) {
            return std::nullopt;
        }
        const auto payload = cursor.read_bytes(*size);
        if (!payload) {
            return std::nullopt;
        }

        switch (*type) {
            case RECORD_VALUE: {
                if (payload->size() != 2 ||
                    message.values.size() >= MAX_VALUES) {
                    return std::nullopt;
                }
                const auto high = std::to_integer<std::uint16_t>((*payload)[0]);
                const auto low = std::to_integer<std::uint16_t>((*payload)[1]);
                message.values.push_back(
                    static_cast<std::uint16_t>((high << BITS_PER_BYTE) | low));
                break;
            }
            case RECORD_NAME:
                if (payload->size() > MAX_NAME_SIZE) {
                    return std::nullopt;
                }
                message.name = to_text(*payload);
                break;
            case RECORD_LABEL: {
                std::optional<std::string> label = copy_label(*payload);
                if (!label) {
                    return std::nullopt;
                }
                message.label = std::move(*label);
                break;
            }
            default:
                return std::nullopt;
        }
    }
    return message;
}

}  // namespace scada::toy
