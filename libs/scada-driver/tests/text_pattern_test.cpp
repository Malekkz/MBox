// The text pattern of identity checks (configuration schemas contract 2.5,
// 4.8; decision D-147).

#include "scada/driver/text_pattern.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the texts are the test data

TEST(TextPattern, MatchesTheWholeText) {
    EXPECT_TRUE(text_matches("SUN2000", "SUN2000"));
    EXPECT_FALSE(text_matches("SUN2000", "SUN2000-10KTL"));
    EXPECT_FALSE(text_matches("SUN2000", "XSUN2000"));
    EXPECT_TRUE(text_matches("", ""));
    EXPECT_FALSE(text_matches("", "a"));
}

TEST(TextPattern, AStarTakesAnyCharactersAQuestionMarkOne) {
    EXPECT_TRUE(text_matches("SUN2000-*KTL", "SUN2000-10KTL"));
    EXPECT_TRUE(text_matches("SUN2000-*KTL", "SUN2000-KTL"));
    EXPECT_FALSE(text_matches("SUN2000-*KTL", "SUN2000-10KTL-M1"));
    EXPECT_TRUE(text_matches("*", ""));
    EXPECT_TRUE(text_matches("**", "abc"));
    EXPECT_TRUE(text_matches("v?.?", "v1.2"));
    EXPECT_FALSE(text_matches("v?.?", "v1.23"));
    EXPECT_FALSE(text_matches("?", ""));
    EXPECT_TRUE(text_matches("*a*a*", "banana"));
    EXPECT_FALSE(text_matches("*a*a*a*a*", "banana"));
}

TEST(TextPattern, ABackslashMakesTheNextCharacterLiteral) {
    EXPECT_TRUE(text_matches(R"(a\*b)", "a*b"));
    EXPECT_FALSE(text_matches(R"(a\*b)", "axb"));
    EXPECT_TRUE(text_matches(R"(\?)", "?"));
    EXPECT_FALSE(text_matches(R"(\?)", "x"));
    EXPECT_TRUE(text_matches(R"(a\\b)", R"(a\b)"));
    // A backslash at the end stands for itself.
    EXPECT_TRUE(text_matches(R"(a\)", R"(a\)"));
    EXPECT_TRUE(text_matches(R"(*\*)", "x*"));
    EXPECT_FALSE(text_matches(R"(*\*)", "x"));
}

TEST(TextPattern, TheLastPartDoesNotReuseWhatTheOthersTook) {
    // "ab" is taken by the first part; the last needs its own "b".
    EXPECT_FALSE(text_matches("ab*b", "ab"));
    EXPECT_TRUE(text_matches("ab*b", "abb"));
    EXPECT_FALSE(text_matches("a*bc*cd", "abcd"));
    EXPECT_TRUE(text_matches("a*bc*cd", "abccd"));
}

// The plain definition, by the textbook table: the oracle. The pattern is
// first split into elements, an escaped character being one literal. Fixed
// buffers, for the short strings of these tests: no allocation per call.
constexpr std::size_t ORACLE_MAX = 16;

bool reference(std::string_view pattern, std::string_view text) {
    struct Element {
        bool star = false;
        bool any = false;
        char literal = 0;
    };
    if (pattern.size() > ORACLE_MAX || text.size() > ORACLE_MAX) {
        ADD_FAILURE() << "the oracle takes at most " << ORACLE_MAX << " bytes";
        return false;
    }
    std::array<Element, ORACLE_MAX> elements{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        Element& element = elements.at(count++);
        if (pattern[index] == '\\' && index + 1 < pattern.size()) {
            element.literal = pattern[++index];
        } else if (pattern[index] == '*') {
            element.star = true;
        } else if (pattern[index] == '?') {
            element.any = true;
        } else {
            element.literal = pattern[index];
        }
    }
    // `matched[j]`: the elements so far match the first j bytes of text.
    std::array<bool, ORACLE_MAX + 1> matched{};
    matched[0] = true;
    for (std::size_t each = 0; each < count; ++each) {
        const Element& element = elements.at(each);
        std::array<bool, ORACLE_MAX + 1> next{};
        for (std::size_t length = 0; length <= text.size(); ++length) {
            if (element.star) {
                next.at(length) =
                    matched.at(length) || (length > 0 && next.at(length - 1));
            } else if (length > 0 && matched.at(length - 1)) {
                next.at(length) =
                    element.any || text[length - 1] == element.literal;
            }
        }
        matched = next;
    }
    return matched.at(text.size());
}

// Every string of 0 to `length` characters of `alphabet`, counted up like
// an odometer.
std::vector<std::string> every(std::string_view alphabet, std::size_t length) {
    std::vector<std::string> out{std::string()};
    for (std::size_t size = 1; size <= length; ++size) {
        std::vector<std::size_t> digits(size, 0);
        for (;;) {
            std::string word;
            for (const std::size_t digit : digits) {
                word.push_back(alphabet[digit]);
            }
            out.push_back(std::move(word));
            std::size_t place = 0;
            while (place < size && ++digits[place] == alphabet.size()) {
                digits[place] = 0;
                ++place;
            }
            if (place == size) {
                break;
            }
        }
    }
    return out;
}

TEST(TextPattern, AgreesWithTheDefinitionOnEveryShortPatternAndText) {
    const std::vector<std::string> patterns = every("ab*?\\", 5);
    const std::vector<std::string> texts = every("ab*", 5);
    std::size_t compared = 0;
    for (const std::string& pattern : patterns) {
        for (const std::string& text : texts) {
            // Plain checks: a million gtest assertions would cost seconds.
            std::size_t comparisons = 0;
            const bool matched =
                text_matches_counted(pattern, text, comparisons);
            if (matched != reference(pattern, text) ||
                comparisons > (text.size() + 1) * (pattern.size() + 1)) {
                FAIL() << "pattern '" << pattern << "' text '" << text
                       << "': matched " << matched << " in " << comparisons
                       << " comparisons";
            }
            ++compared;
        }
    }
    EXPECT_GT(compared, 1'000'000U);
}

TEST(TextPattern, TheWorstCaseStaysWithinItsBound) {
    // One star and a literal run that fits almost everywhere, on a text of
    // the longest STRING: every retry of the star compares nearly the whole
    // run, the case that costs most.
    const std::string pattern = "*" + std::string(100, 'a') + "b";
    const std::string text(250, 'a');
    std::size_t comparisons = 0;
    EXPECT_FALSE(text_matches_counted(pattern, text, comparisons));
    EXPECT_LE(comparisons, (text.size() + 1) * (pattern.size() + 1));
    EXPECT_GT(comparisons, 100 * text.size() / 2);  // quadratic, as stated
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver
