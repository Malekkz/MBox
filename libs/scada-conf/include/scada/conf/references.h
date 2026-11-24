#ifndef SCADA_CONF_REFERENCES_H
#define SCADA_CONF_REFERENCES_H

// Stage 5 of validation, the part that needs no expansion (configuration
// schemas contract, sections 1.4, 10.3 and Annex A.1; decision D-113): names
// that must exist, names defined twice, and the protocol rules of a channel.
// What needs the tags of expanded instances waits for the expander (#103):
// a reference to a tag gives no error here.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"

#include <span>

namespace scada::conf {

// A file that passed stage 3, with the types stage 3 gave its scalars.
struct CheckedFile {
    const TreeFile* source = nullptr;
    const Document* document = nullptr;
    const ScalarTypes* types = nullptr;
};

// Checks `files`, the whole tree, and adds what it finds to `diagnostics`:
// - CF-310 for a template, protocol, area, logging policy, aggregate tier,
//   enumeration, task, program, interlock, output, alarm priority or alarm
//   class that does not exist;
// - CF-303 for a channel, device or interlock name given twice;
// - CF-301 and CF-302 for the tags that tag files and interlocks define;
// - CF-622 for `data_bits: 7` and a loopback, multicast or broadcast host of
//   a Modbus channel;
// - the types of the configuration expressions of templates (CF-523), and
//   CF-310 for a name in them that no parameter or variable of the
//   template's chain has.
// Expects files that passed stage 3: a value of the wrong shape is skipped.
void check_references(std::span<const CheckedFile> files,
                      Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_REFERENCES_H
