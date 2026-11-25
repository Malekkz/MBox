#ifndef SCADA_CONF_SRC_EXPRESSION_SITE_H
#define SCADA_CONF_SRC_EXPRESSION_SITE_H

// Where an error inside an expression is in the file (decision D-109),
// shared by stage 3 (syntax) and stage 5 (names and types).

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"

#include <cstdint>
#include <string>

namespace scada::conf {

// The place of an error at byte `column` (1-based) of the text of `scalar`.
struct ExpressionSite {
    Location location;
    // " at column C" when `location` is the character itself, else " at line
    // L, column C of the expression's text"; C counts characters.
    std::string where;
};

// At the offending character when the scalar's text is its source on one
// line (`Document::verbatim`); otherwise at the scalar, with the line and
// column within the text in `where`. A column past the text counts as its
// end.
[[nodiscard]] ExpressionSite expression_site(const Document& document,
                                             NodeId scalar,
                                             std::uint32_t column);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_EXPRESSION_SITE_H
