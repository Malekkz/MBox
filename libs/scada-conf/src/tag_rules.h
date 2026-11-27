#ifndef SCADA_CONF_SRC_TAG_RULES_H
#define SCADA_CONF_SRC_TAG_RULES_H

// Stage 5 on retained values (configuration schemas contract 5.2), logging
// (6) and alarms (8): decision D-119, slice 7c of #100.

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/references.h"
#include "tag_names.h"

#include <span>

namespace scada::conf {

// Checks the tags of `tree` and `files` (points, bits, tags of tag files),
// `logging.yaml` and `alarms/*.yaml`. Adds to `diagnostics`:
// - CF-505 for a percentage `deadband` of a tag without `range`, given in
//   the tag's alarm or logging, or taken from its logging policy;
// - CF-506 for `retain_now: true` or `retain_max_age` on a tag that is not
//   retained; CF-507 for a retained STRING tag; CF-508 for
//   `retain_max_age` on a counter; CF-509 for `retain_now` on a virtual
//   tag; warning CW-510 for every `retain_max_age`;
// - CF-801 for an aggregate tier whose interval is not longer than the one
//   before, or not a whole multiple of it;
// - CF-802 for logging settings without `policy` that lack `mode` or
//   `raw_retention`; CF-803 for a STRING tag logged other than on change;
// - CF-811 for `priorities`, `max_shelve` or `classes` in a second alarms
//   file; CF-812 for two alarms of one id on one tag; CF-813 for a
//   `high_high` limit not above every `high` limit of its tag, or a `low`
//   limit not above every `low_low` one; CF-814 for an alarm on a STRING
//   tag; CF-815 for a limit alarm on a BOOL tag.
// The rules that read a tag's value type (CF-507, CF-803, CF-814, CF-815)
// are not checked when `tags` is not complete (CF-1001). An error of a
// template's point is at its instance, with the template note, and one
// cause at many instances is one error (D-114).
void check_tag_rules(const TagNames& tags, const ExpandedTree& tree,
                     std::span<const CheckedFile> files,
                     Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_TAG_RULES_H
