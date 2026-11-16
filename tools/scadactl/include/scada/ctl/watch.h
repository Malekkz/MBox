#ifndef SCADA_CTL_WATCH_H
#define SCADA_CTL_WATCH_H

// `scadactl tag watch`: the current value of every matching tag, then
// every change from the change rings of their owners (tag database
// contract, section 8), until stopped.

#include "scada/ctl/arguments.h"
#include "scada/ctl/database.h"

#include <iosfwd>

namespace scada::ctl {

// Watches the tags of `database` that match `args.pattern`, writing lines to
// `out` (text, or JSON with `args.json`) and messages to `err`. Returns
// the exit code: EXIT_OK when stopped by SIGINT or SIGTERM or after
// `args.count` changes, EXIT_ERROR if no tag matches, a subscription
// cannot be opened, or the database cannot be observed again after a
// generation change.
//
// After a gap it writes a gap line and reads the owner's tags again, and
// drops changes older than what it read. When scada_tag_status reports a
// generation change it observes again, writes a generation line and starts
// over with the tags of the new generation. See scadactl.h for signals.
[[nodiscard]] int watch(Database& database, const Arguments& args,
                        std::ostream& out, std::ostream& err);

}  // namespace scada::ctl

#endif  // SCADA_CTL_WATCH_H
