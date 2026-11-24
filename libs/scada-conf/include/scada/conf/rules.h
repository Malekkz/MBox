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

}  // namespace scada::conf

#endif  // SCADA_CONF_RULES_H
