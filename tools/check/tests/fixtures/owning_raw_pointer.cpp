// A raw pointer that owns memory: rejected by clang-tidy
// (cppcoreguidelines-owning-memory) and by the banned list.

namespace fixture {

int read_through_owner(int value) {
    int* owner = new int(value);  // expect: scada-banned
    const int result = *owner;
    delete owner;  // expect: scada-banned
    return result;
}

}  // namespace fixture
