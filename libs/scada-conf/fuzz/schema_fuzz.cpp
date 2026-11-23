// Fuzz target: stage 3, the schema validator (configuration schemas contract
// 1.2, 10.2). The input is one file; once it parses, it is validated against
// every built-in schema. Every error must point inside the input.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/schema.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string text(bytes.begin(), bytes.end());
    std::size_t lines = 1;
    for (const char chr : text) {
        if (chr == '\n') {
            ++lines;
        }
    }
    for (const std::string_view schema :
         {"system.schema.json", "areas.schema.json", "enums.schema.json",
          "logging.schema.json", "tasks.schema.json"}) {
        scada::conf::Diagnostics diagnostics;
        std::optional<scada::conf::Document> document =
            scada::conf::parse_yaml(text, "f.yaml", diagnostics);
        if (!document) {
            return 0;
        }
        scada::conf::ScalarTypes types(*document);
        scada::conf::SchemaSet::builtin().validate(*document, types, schema,
                                                   "f.yaml", diagnostics);
        if (types.size() != document->size()) {
            std::abort();
        }
        scada::conf::Report report;
        diagnostics.move_into(report);
        for (const scada::conf::Diagnostic& error : report.errors) {
            if (error.location.line < 1 || error.location.line > lines ||
                error.location.column < 1 || error.message.empty()) {
                std::abort();
            }
        }
    }
    return 0;
}
