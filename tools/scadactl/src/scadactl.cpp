#include "scada/ctl/scadactl.h"

#include "scada/ctl/arguments.h"
#include "scada/ctl/database.h"
#include "scada/ctl/view.h"
#include "scada/ctl/watch.h"
#include "scada/tag/scada_tag.h"

#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::ctl {

namespace {

// Looks at the generation before reading: observes again if it changed
// since the context was opened.
bool fresh_generation(Database& database, std::ostream& err) {
    scada_status status{};
    if (database.status(status) != SCADA_E_RETIRING) {
        return true;
    }
    std::string detail;
    if (!database.reopen(detail)) {
        err << "scadactl: " << detail << "\n";
        return false;
    }
    return true;
}

int show_status(Database& database, const Arguments& args, std::ostream& out) {
    const StatusView view = database.status_view();
    std::string text;
    if (args.json) {
        render_status_json(text, view);
    } else {
        render_status_text(text, view);
    }
    out << text << std::flush;
    return EXIT_OK;
}

int list_tags(Database& database, const Arguments& args, std::ostream& out) {
    const std::vector<TagRef> tags = database.find(args.pattern);
    std::vector<TagRow> rows;
    rows.reserve(tags.size());
    for (const TagRef& tag : tags) {
        rows.push_back(database.read(tag));
    }
    std::string text;
    if (args.json) {
        scada_status status{};
        static_cast<void>(database.status(status));
        render_tag_list_json(text, status.generation, rows);
    } else if (rows.empty()) {
        text = "no tag matches\n";
    } else {
        render_tag_table(text, rows);
    }
    out << text << std::flush;
    return EXIT_OK;
}

int get_tag(Database& database, const Arguments& args, std::ostream& out,
            std::ostream& err) {
    const std::optional<TagRef> tag = database.find_one(args.name);
    if (!tag.has_value()) {
        // The name was checked against the grammar: safe to print.
        err << "scadactl: there is no tag " << args.name << "\n";
        return EXIT_ERROR;
    }
    const TagRow row = database.read(*tag);
    std::string text;
    if (args.json) {
        render_tag_json(text, row);
        text.push_back('\n');
    } else {
        render_tag_detail(text, row);
    }
    out << text << std::flush;
    return EXIT_OK;
}

}  // namespace

int run(std::span<const std::string_view> args, std::ostream& out,
        std::ostream& err) {
    std::string detail;
    const std::optional<Arguments> parsed = parse_arguments(args, detail);
    if (!parsed.has_value()) {
        err << "scadactl: " << detail << "\nTry 'scadactl --help'.\n";
        return EXIT_USAGE;
    }
    if (parsed->command == Command::HELP) {
        out << USAGE << std::flush;
        return EXIT_OK;
    }
    const std::unique_ptr<Database> database = Database::open(*parsed, detail);
    if (database == nullptr) {
        err << "scadactl: " << detail << "\n";
        return EXIT_ERROR;
    }
    if (!fresh_generation(*database, err)) {
        return EXIT_ERROR;
    }
    switch (parsed->command) {
        case Command::STATUS:
            return show_status(*database, *parsed, out);
        case Command::TAG_LIST:
            return list_tags(*database, *parsed, out);
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
