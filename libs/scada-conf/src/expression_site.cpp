#include "expression_site.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scada::conf {

namespace {

// A UTF-8 continuation byte is 10xxxxxx; every other byte starts a character.
constexpr unsigned char UTF8_CONTINUATION_MASK = 0xC0;
constexpr unsigned char UTF8_CONTINUATION = 0x80;

}  // namespace

ExpressionSite expression_site(const Document& document, NodeId scalar,
                               std::uint32_t column) {
    const std::string_view text = document.text(scalar);
    const std::size_t offset =
        std::min<std::size_t>(column > 0 ? column - 1 : 0, text.size());
    std::size_t line = 1;
    std::size_t character = 1;
    for (std::size_t i = 0; i < offset; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == '\n') {
            ++line;
            character = 1;
        } else if ((byte & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION) {
            ++character;
        }
    }
    ExpressionSite site{.location = document.location(scalar),
                        .where = " at column " + std::to_string(character)};
    if (document.verbatim(scalar)) {
        const bool quoted =
            document.style(scalar) == ScalarStyle::SINGLE_QUOTED ||
            document.style(scalar) == ScalarStyle::DOUBLE_QUOTED;
        site.location.column +=
            static_cast<std::uint32_t>(character - 1 + (quoted ? 1 : 0));
    } else {
        site.where = " at line " + std::to_string(line) + ", column " +
                     std::to_string(character) + " of the expression's text";
    }
    return site;
}

}  // namespace scada::conf
