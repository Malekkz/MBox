// An implicit narrowing conversion: rejected by clang-tidy
// (cppcoreguidelines-narrowing-conversions).

namespace fixture {

int truncate(double value) {
    const int whole = value;
    return whole;
}

}  // namespace fixture
