#include "scada/ctl/scadactl.h"

#include "scada/ctl/arguments.h"
#include "scada/ctl/database.h"
#include "scada/ctl/view.h"
#include "scada/ctl/watch.h"
#include "scada/tag/scada_tag.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::ctl {

namespace {

// Reads with a generation change no more often than this before giving up.
constexpr int READ_ATTEMPTS = 3;

// The result of reading under a check of the generation.
struct Checked {
    bool ok = false;
    bool tagd_down = false;
    std::uint64_t generation = 0;
};

// Runs `read`, then asks scada_tag_status whether the generation it read
// from is still the current one (contract 11.4): if it was retired, observes
// again and runs `read` again, so that nothing of a retired generation is
// shown. `read` returns false when there is nothing to show.
template <typename Read>
Checked read_checked(Database& database, std::ostream& err, Read read) {
    Checked checked;
    for (int attempt = 0; attempt < READ_ATTEMPTS; ++attempt) {
        if (!read()) {
            return checked;
        }
        scada_status status{};
        const int code = database.status(status);
        if (code != SCADA_E_RETIRING) {
            checked.ok = true;
            checked.tagd_down = code == SCADA_E_TAGD_DOWN;
            checked.generation = status.generation;
            return checked;
        }
        std::string detail;
        if (database.reopen(detail) != Database::Reopened::YES) {
            err << "scadactl: " << detail << "\n";
            return checked;
        }
    }
    err << "scadactl: the generation kept changing while it was read; try "
           "again\n";
    return checked;
}

int show_status(Database& database, const Arguments& args, std::ostream& out,
                std::ostream& err) {
    StatusView view;
    const Checked checked = read_checked(database, err, [&] {
        view = database.status_view();
        return true;
    });
    if (!checked.ok) {
        return EXIT_ERROR;
    }
    std::string text;
    if (args.json) {
        render_status_json(text, view);
    } else {
        render_status_text(text, view);
    }
    return write_out(out, err, text) ? EXIT_OK : EXIT_ERROR;
}

int list_tags(Database& database, const Arguments& args, std::ostream& out,
              std::ostream& err) {
    std::vector<TagRow> rows;
    const Checked checked = read_checked(database, err, [&] {
        rows.clear();
        for (const TagRef& tag : database.find(args.pattern)) {
            rows.push_back(database.read(tag));
        }
        return true;
    });
    if (!checked.ok) {
        return EXIT_ERROR;
    }
    std::string text;
    if (args.json) {
        render_tag_list_json(text, checked.generation, checked.tagd_down, rows);
    } else {
        if (checked.tagd_down) {
            render_tagd_warning(text);
        }
        if (rows.empty()) {
            text.append("no tag matches\n");
        } else {
            render_tag_table(text, rows);
        }
    }
    return write_out(out, err, text) ? EXIT_OK : EXIT_ERROR;
}

int get_tag(Database& database, const Arguments& args, std::ostream& out,
            std::ostream& err) {
    TagRow row;
    bool found = false;
    const Checked checked = read_checked(database, err, [&] {
        const std::optional<TagRef> tag = database.find_one(args.name);
        found = tag.has_value();
        if (found) {
            row = database.read(*tag);
        }
        return found;
    });
    if (!found) {
        // The name was checked against the grammar: safe to print.
        err << "scadactl: there is no tag " << args.name << "\n";
        return EXIT_ERROR;
    }
    if (!checked.ok) {
        return EXIT_ERROR;
    }
    std::string text;
    if (args.json) {
        render_tag_get_json(text, row, checked.tagd_down);
    } else {
        if (checked.tagd_down) {
            render_tagd_warning(text);
        }
        render_tag_detail(text, row);
    }
    return write_out(out, err, text) ? EXIT_OK : EXIT_ERROR;
}

}  // namespace

bool write_out(std::ostream& out, std::ostream& err, std::string_view text) {
    out << text << std::flush;
    if (out) {
        return true;
    }
    err << "scadactl: writing the output failed\n";
    return false;
}

int run(std::span<const std::string_view> args, std::ostream& out,
        std::ostream& err) {
    std::string detail;
    const std::optional<Arguments> parsed = parse_arguments(args, detail);
    if (!parsed.has_value()) {
        err << "scadactl: " << detail << "\nTry 'scadactl --help'.\n";
        return EXIT_USAGE;
    }
    if (parsed->command == Command::HELP) {
        return write_out(out, err, USAGE) ? EXIT_OK : EXIT_ERROR;
    }
    const std::unique_ptr<Database> database = Database::open(*parsed, detail);
    if (database == nullptr) {
        err << "scadactl: " << detail << "\n";
        return EXIT_ERROR;
    }
    switch (parsed->command) {
        case Command::STATUS:
            return show_status(*database, *parsed, out, err);
        case Command::TAG_LIST:
            return list_tags(*database, *parsed, out, err);
        case Command::TAG_GET:
            return get_tag(*database, *parsed, out, err);
        case Command::TAG_WATCH:
            return watch(*database, *parsed, out, err);
        case Command::HELP:
            break;
    }
    return EXIT_OK;
}

}  // namespace scada::ctl
