#ifndef SCADA_CONF_RULES_H
#define SCADA_CONF_RULES_H

// The rules of stage 3 that a schema cannot state (configuration schemas
// contract 1.4; D-108): keys required or refused by the value of another key
// of one object that no merge can complete, and checks across the items of
// one value. They read a file after its schema check, with the types it
// gave.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"

#include <optional>
#include <string>
#include <string_view>

namespace scada::conf {

// Adds to `diagnostics` the errors of these rules in `document`, a file of
// `type` whose protocol is `protocol` (a channel file's directory, a
// template's `protocol` key; empty for any other file, or for a protocol
// this version does not know). `types` is the table the schema check filled
// for `document`. Every error is a CF-2xx code or CF-517, at the key or
// object it concerns. A part of the file of the wrong kind is skipped: the
// schema check has reported it.
void check_file_rules(const Document& document, const ScalarTypes& types,
                      FileType type, std::string_view protocol,
                      Diagnostics& diagnostics);

// True if `node` of `document`, typed by `types`, is the integer 1 in any of
// its written forms (`1`, `0x1`; contract 1.2): `divide_by: 1` is an
// integer scaling (4.7). Stage 3 and the merged-point checks of stage 5 read
// a divisor with it.
[[nodiscard]] bool is_integer_one(const Document& document,
                                  const ScalarTypes& types, NodeId node);

// The error of the bit field `field` of `document` (`[lo, hi]`, contract
// 4.7, A.2) whose two items are integers with lo above hi: its message,
// `path` naming the field; nothing otherwise. Stage 3 applies it to a
// written field, stage 5 to one with a computed item.
[[nodiscard]] std::optional<std::string> bit_field_order_error(
    const Document& document, const ScalarTypes& types, NodeId field,
    std::string_view path);

}  // namespace scada::conf

#endif  // SCADA_CONF_RULES_H
