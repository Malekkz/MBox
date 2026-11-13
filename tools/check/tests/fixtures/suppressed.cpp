// Suppressions (coding standard, section 14): only one that names the check
// is honoured by the banned-function and SCADA_RT checks.

#include <cstring>

namespace fixture {

std::size_t lengths(const char* text) {
    // NOLINTNEXTLINE(scada-banned): fixture, suppression on the next line
    std::size_t total = std::strlen(text);
    total += std::strlen(text);  // NOLINT(scada-banned): fixture, same line
    total += std::strlen(text);  // NOLINT(misc-x, scada-banned): in a list
    total += std::strlen(text);  // NOLINT  // expect: scada-banned
    total += std::strlen(text);  // NOLINT(scada-rt)  // expect: scada-banned
    return total;
}

}  // namespace fixture
