// Fuzz target: the YAML subset parser (configuration schemas contract 1.1,
// 1.3). The input is the bytes of one file. A parse gives a document or
// exactly one error, and every node of a document lies within the input.

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/yaml.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

// Smaller limits than the contract's, so that the fuzzer reaches them.
constexpr std::size_t FUZZ_NODES = 2000;
constexpr std::size_t FUZZ_ENTRIES = 100;
constexpr std::size_t FUZZ_SCALAR = 1024;

void check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void check_document(const scada::conf::Document& document,
                    std::size_t input_lines) {
    using scada::conf::Node;
    using scada::conf::NodeKind;
    check(document.size() > 0);
    for (scada::conf::NodeId id = 0; id < document.size(); ++id) {
        const Node& node = document.node(id);
        check(node.line >= 1 && node.line <= input_lines);
        check(node.column >= 1);
        if (node.kind == NodeKind::MAP) {
            check(node.children.size() % 2 == 0);
            for (std::size_t i = 0; i < node.children.size(); i += 2) {
                check(document.node(node.children.at(i)).kind ==
                      NodeKind::SCALAR);
            }
        }
        for (const scada::conf::NodeId child : node.children) {
            check(child > id && child < document.size());
        }
        if (node.kind != NodeKind::SCALAR) {
            check(node.text.empty());
        }
    }
}

void parse_once(std::string_view text, const scada::conf::ParseLimits& limits) {
    scada::conf::Diagnostics diagnostics;
    const std::optional<scada::conf::Document> document =
        scada::conf::parse_yaml(text, "f.yaml", diagnostics, limits);
    std::size_t lines = 1;
    for (const char chr : text) {
        if (chr == '\n') {
            ++lines;
        }
    }
    if (document) {
        check(diagnostics.error_count() == 0);
        check_document(*document, lines);
    } else {
        check(diagnostics.error_count() == 1);
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    const std::string text(bytes.begin(), bytes.end());
    parse_once(text, scada::conf::ParseLimits{});
    scada::conf::ParseLimits small;
    small.max_nodes = FUZZ_NODES;
    small.max_entries = FUZZ_ENTRIES;
    small.max_scalar_bytes = FUZZ_SCALAR;
    parse_once(text, small);
    return 0;
}
