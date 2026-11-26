#ifndef SCADA_CONF_SRC_GROUPED_ERRORS_H
#define SCADA_CONF_SRC_GROUPED_ERRORS_H

// One cause met at many instances is one error (configuration schemas
// contract 1.4; the ruling O-6 on #146, D-114): the errors that a template's
// line causes, grouped by cause, for the expander (stage 4) and the checks
// of a merged point (stage 5, D-115).

#include "scada/conf/diagnostic.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

class GroupedErrors {
public:
    // Keeps at most `kept_limit` diagnostics; past it an error is counted
    // only.
    explicit GroupedErrors(std::size_t kept_limit) : kept_limit_(kept_limit) {}

    // Keeps `diagnostic`, or counts it in `diagnostics`. One whose first
    // note is a template's is a cause: its code, the note's place and `key`
    // (the message without what names the instance). Met again at another
    // instance, the place the diagnostic is at, it is counted with
    // `instance`, the name of that instance, and the first names the others
    // when flushed. A diagnostic of the same text as one kept is dropped.
    // With `groupable` false it is never a cause: an error that the
    // instance's own file contributes to is its own (item 1 of the delta
    // review of #149).
    void add(Diagnostic diagnostic, std::string_view key,
             std::string_view instance, Diagnostics& diagnostics,
             bool groupable = true);

    // Gives the kept diagnostics to `diagnostics` in the order they came,
    // the first of a cause with "(and N more instances: <the first 10>)",
    // and starts again empty.
    void flush(Diagnostics& diagnostics);

    // The texts kept to drop repeats, and the causes: for tests of their
    // bound, which is the kept limit.
    [[nodiscard]] std::size_t kept_texts() const noexcept {
        return reported_.size();
    }
    [[nodiscard]] std::size_t causes() const noexcept { return causes_.size(); }

private:
    struct Cause {
        std::size_t first = 0;           // in `pending_`
        std::size_t more = 0;            // other instances
        Location last;                   // the instance it was last met at
        std::vector<std::string> names;  // of the first other instances
    };

    std::size_t kept_limit_;
    std::vector<Diagnostic> pending_;
    std::map<std::string, Cause> causes_;
    std::set<std::string> reported_;
};

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_GROUPED_ERRORS_H
