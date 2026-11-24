#ifndef SCADA_CONF_DIAGNOSTIC_H
#define SCADA_CONF_DIAGNOSTIC_H

// Errors and warnings of configuration validation (configuration schemas
// contract, sections 1.4 and 10).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

// A place in a file of the tree. `file` is the path relative to the tree's
// root, `/` as separator. Line and column count from 1; a diagnostic about a
// whole file, a directory or the tree points at line 1, column 1 (D-105).
struct Location {
    std::string file;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

// An error (`CF-<n>`, blocks a commit) or a warning (`CW-<n>`).
enum class Severity : std::uint8_t { ERROR, WARNING };

// One reported finding: where, which rule, and the rule's message with its
// placeholders filled (contract section 10).
struct Diagnostic {
    Severity severity = Severity::ERROR;
    // The number of the code: 101 for CF-101.
    std::uint16_t code = 0;
    Location location;
    std::string message;
    // Further places that explain the first one, in order. #103 uses them
    // for "from template <file>:<line>:<column> (<detail>)" (section 1.4).
    struct Note {
        // What the place is, for example "template".
        std::string kind;
        Location location;
        // Optional, for example "point flow".
        std::string detail;
    };
    std::vector<Note> notes;
};

// The most bytes of a message, a path, a note's kind or its detail as a
// Diagnostics object keeps it (D-110); longer text is cut at a character and
// ends in `...`.
constexpr std::size_t MAX_DIAGNOSTIC_TEXT_BYTES = 1024;

// Returns `text`, which may hold any bytes of a file or a path, in a form
// that prints as itself on one line and cannot drive a terminal or hide or
// reorder characters (D-110): line feed, carriage return and tab as `\n`,
// `\r`, `\t`; other C0 control characters, DEL and bytes that are not valid
// UTF-8 as `\xNN`; C1 control characters, invisible and bidirectional
// characters, the non-characters and the tag characters as `\uNNNN`, or
// `\UNNNNNNNN` above U+FFFF. Everything else is kept, a backslash too, so
// that the function changes nothing it has already made printable. At most
// `max_bytes` bytes (at least 3): longer text is cut before a whole
// character or escape and ends in `...`.
[[nodiscard]] std::string printable(
    std::string_view text, std::size_t max_bytes = MAX_DIAGNOSTIC_TEXT_BYTES);

// The most bytes of a file's text that `quote_text` puts in a message, so
// that the rest of the message stays within MAX_DIAGNOSTIC_TEXT_BYTES.
constexpr std::size_t MAX_QUOTED_BYTES = 64;

// Returns `text` in double quotes, with `\` and `"` written `\\` and `\"`,
// so that a quoted piece of a file in a message has an unambiguous end. Text
// over `max_bytes` is cut before a whole character and is followed by `...`
// after the closing quote. For keys and values in messages; `Diagnostics::add`
// then makes it printable.
[[nodiscard]] std::string quote_text(std::string_view text,
                                     std::size_t max_bytes = MAX_QUOTED_BYTES);

// Returns the code as written, for example "CF-101" or "CW-510".
[[nodiscard]] std::string code_text(const Diagnostic& diagnostic);

// Returns the diagnostic in the form of contract section 1.4:
// `<file>:<line>:<column>: error CF-<n>: <message>`, then one line per note,
// indented by two spaces: `from <kind> <file>:<line>:<column> (<detail>)`,
// the part in brackets only with a detail. No final line end.
[[nodiscard]] std::string format(const Diagnostic& diagnostic);

// True if `lhs` comes before `rhs` in the order of contract section 1.4: file
// path (bytes), line, column, code. Ties keep their order (a stable sort).
[[nodiscard]] bool reported_before(const Diagnostic& lhs,
                                   const Diagnostic& rhs) noexcept;

// The result of a validation run: the first `REPORTED_LIMIT` errors and
// warnings in the order of section 1.4, and how many there were in all.
struct Report {
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;
    std::size_t error_count = 0;
    std::size_t warning_count = 0;
};

// True if the tree of `report` may be committed: no error (warnings may
// remain).
[[nodiscard]] inline bool passed(const Report& report) noexcept {
    return report.error_count == 0;
}

// Collects the diagnostics of one validation stage.
//
// Memory is bounded whatever the tree: only the first `REPORTED_LIMIT` of
// each severity in the order of section 1.4 are kept (a hostile tree can
// produce any number), plus the counts. The kept set does not depend on the
// order in which diagnostics are added, so the same tree always reports the
// same first 1,000 (section 1.4).
class Diagnostics {
public:
    // Section 1.3: the first 1,000 of each, then a count.
    static constexpr std::size_t REPORTED_LIMIT = 1000;

    // Adds an error CF-<code>.
    void error(std::uint16_t code, Location location, std::string message);

    // Adds a warning CW-<code>.
    void warning(std::uint16_t code, Location location, std::string message);

    // Adds `diagnostic`, with its message, paths, kinds and details made
    // `printable`: every diagnostic passes here, so that no text of a file
    // reaches a report unescaped.
    void add(Diagnostic diagnostic);

    // Counts `count` errors that were found but not kept, for a caller that
    // capped its own list before adding (a `oneOf` branch, D-107).
    void count_unlisted_errors(std::size_t count) noexcept {
        error_count_ += count;
    }

    // How many errors and warnings were added, also those beyond the limit.
    [[nodiscard]] std::size_t error_count() const noexcept {
        return error_count_;
    }
    [[nodiscard]] std::size_t warning_count() const noexcept {
        return warning_count_;
    }

    // Moves everything collected into `report`, sorted, and leaves this
    // object empty. What `report` already holds comes first: a later stage
    // appends to the report of an earlier one (section 1.4 sorts within a
    // stage). The report keeps at most `REPORTED_LIMIT` of each severity in
    // all; the counts add up.
    void move_into(Report& report);

private:
    // Keeps the `REPORTED_LIMIT` first of `list` in order once it has grown
    // to twice that, so that memory stays bounded.
    static void trim(std::vector<Diagnostic>& list, bool force);

    std::vector<Diagnostic> errors_;
    std::vector<Diagnostic> warnings_;
    std::size_t error_count_ = 0;
    std::size_t warning_count_ = 0;
};

}  // namespace scada::conf

#endif  // SCADA_CONF_DIAGNOSTIC_H
