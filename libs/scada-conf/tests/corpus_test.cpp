// The conformance corpus of configuration schemas contract section 14: each
// case is a tree and the list of (file, line, column, code) it must give, in
// the order of section 1.4, and for a valid tree its expanded points if the
// case pins them. See corpus/README.md.

#include "expanded_text.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace scada::conf {
namespace {

namespace fs = std::filesystem;

std::vector<fs::path> case_directories() {
    std::vector<fs::path> cases;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(SCADA_CONF_CORPUS_DIR)) {
        if (entry.is_directory()) {
            cases.push_back(entry.path());
        }
    }
    std::ranges::sort(cases);
    return cases;
}

std::vector<std::string> expected_lines(const fs::path& file) {
    std::ifstream input(file);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.front() != '#') {
            lines.push_back(line);
        }
    }
    return lines;
}

// `<file>:<line>:<column>: <code>`, the part of a diagnostic the corpus
// pins.
std::string corpus_line(const Diagnostic& diagnostic) {
    return diagnostic.location.file + ":" +
           std::to_string(diagnostic.location.line) + ":" +
           std::to_string(diagnostic.location.column) + ": " +
           code_text(diagnostic);
}

// The diagnostics of a tree as the corpus pins them, and its expanded
// points when it is valid.
struct Outcome {
    std::vector<std::string> diagnostics;
    std::vector<std::string> points;
};

Outcome outcome_of(const fs::path& tree) {
    const Result<Validation> validation = validate_and_expand(tree);
    EXPECT_TRUE(validation.ok()) << validation.status().message();
    Outcome got;
    if (!validation.ok()) {
        return got;
    }
    const Report& report = validation.value().report;
    for (const Diagnostic& diagnostic : report.errors) {
        got.diagnostics.push_back(corpus_line(diagnostic));
    }
    if (passed(report)) {
        for (const Diagnostic& diagnostic : report.warnings) {
            got.diagnostics.push_back(corpus_line(diagnostic));
        }
    }
    if (const std::optional<ExpandedTree>& expanded =
            validation.value().expanded) {
        got.points = points_of(*expanded, true);
    }
    return got;
}

void check_case(const fs::path& dir) {
    const Outcome got = outcome_of(dir / "tree");
    EXPECT_EQ(got.diagnostics, expected_lines(dir / "expected"));
    // A valid case may pin its expanded points (test 6).
    if (fs::is_regular_file(dir / "expanded")) {
        EXPECT_EQ(got.points, expected_lines(dir / "expanded"));
    }
}

TEST(Corpus, EveryCaseGivesItsExpectedDiagnostics) {
    const std::vector<fs::path> cases = case_directories();
    ASSERT_FALSE(cases.empty());
    for (const fs::path& dir : cases) {
        SCOPED_TRACE(dir.filename().string());
        ASSERT_TRUE(fs::is_directory(dir / "tree")) << "no tree/ in " << dir;
        ASSERT_TRUE(fs::is_regular_file(dir / "expected"))
            << "no expected in " << dir;
        check_case(dir);
    }
}

}  // namespace
}  // namespace scada::conf
