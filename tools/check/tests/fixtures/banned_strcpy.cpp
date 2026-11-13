// strcpy is on the banned list (coding standard, section 12).

#include <cstring>

namespace fixture {

void copy_name(char* destination, const char* source) {
    std::strcpy(destination, source);  // expect: scada-banned
}

}  // namespace fixture
