#ifndef SCADA_COMMON_STATIC_STRING_H
#define SCADA_COMMON_STATIC_STRING_H

#include <cstddef>
#include <string_view>

namespace scada {

// A view of text with static storage duration, in practice a string literal.
//
// It exists so that types such as `Status` can carry a message without
// owning or allocating memory and without the risk of a dangling view: the
// only way to make a non-empty StaticString is from a character array in a
// constant expression, and a constant expression cannot refer to a local
// object.
class StaticString {
public:
    // An empty string.
    constexpr StaticString() noexcept = default;

    // Wraps a string literal. Implicit on purpose, so that a literal can be
    // passed wherever a StaticString is expected. Evaluated at compile time
    // only; passing anything but a constant character array does not compile.
    template <std::size_t N>
    // NOLINTNEXTLINE(google-explicit-constructor,cppcoreguidelines-avoid-c-arrays): a string literal is a C array, and the conversion must be implicit to be usable
    consteval StaticString(const char (&literal)[N]) noexcept
        : view_(literal, literal[N - 1] == '\0' ? N - 1 : N) {}

    // Returns the text. The view is valid for the life of the process.
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return view_;
    }

private:
    std::string_view view_;
};

}  // namespace scada

#endif  // SCADA_COMMON_STATIC_STRING_H
