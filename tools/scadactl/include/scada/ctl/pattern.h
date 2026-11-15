#ifndef SCADA_CTL_PATTERN_H
#define SCADA_CTL_PATTERN_H

// The tag-name patterns of `scadactl tag list` and `scadactl tag watch`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scada::ctl {

// The longest pattern, in bytes: a tag name is at most 127 bytes (tag
// database contract, section 14), and a pattern longer than that would
// match nothing that a shorter one could not.
inline constexpr std::size_t PATTERN_MAX = 127;

// A glob over tag names: `*` stands for any sequence of characters, dots
// included, and `?` for exactly one; every other character stands for
// itself. A pattern without `*` or `?` matches only the name it spells.
class TagPattern {
public:
    // The pattern `text`, or nothing if `text` is empty, longer than
    // PATTERN_MAX or holds a character that is neither in the tag-name
    // alphabet `[a-z0-9_.-]` nor `*` or `?`. Then `detail` says why.
    [[nodiscard]] static std::optional<TagPattern> parse(std::string_view text,
                                                         std::string& detail);

    // The pattern that matches every name.
    [[nodiscard]] static TagPattern all() { return TagPattern("*"); }

    // True if `name` matches the whole pattern. At most the product of the
    // two lengths in steps (127 by 127), with no allocation.
    [[nodiscard]] bool matches(std::string_view name) const noexcept;

    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    explicit TagPattern(std::string text) : text_(std::move(text)) {}

    std::string text_;
};

}  // namespace scada::ctl

#endif  // SCADA_CTL_PATTERN_H
