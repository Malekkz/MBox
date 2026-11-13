// One use of every entry of the banned list (coding standard, section 12).

#include "banned_using.h"

#include <alloca.h>

#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace std::string_literals;  // Allowed: this is not a header.

namespace fixture {

std::mutex guard;

void memory(std::size_t size) {
    int* single = new int(1);  // expect: scada-banned
    delete single;  // expect: scada-banned
    void* block = std::malloc(size);  // expect: scada-banned
    block = std::realloc(block, size * 2);  // expect: scada-banned
    std::free(block);  // expect: scada-banned
    std::free(std::calloc(size, size));  // expect: scada-banned
    void* stack = alloca(size);  // expect: scada-banned
    int variable_length[size];  // expect: scada-banned
    std::memset(stack, 0, size);  // expect: scada-banned
    std::memcpy(stack, variable_length, size);  // expect: scada-banned
    std::memmove(stack, variable_length, size);  // expect: scada-banned
}

void strings(char* destination, const char* source, std::va_list arguments) {
    std::strcpy(destination, source);  // expect: scada-banned
    std::strcat(destination, source);  // expect: scada-banned
    std::sprintf(destination, "%s", source);  // expect: scada-banned
    std::vsprintf(destination, source, arguments);  // expect: scada-banned
    std::strtok(destination, ",");  // expect: scada-banned
    std::strncpy(destination, source, 4);  // expect: scada-banned
    std::strlen(source);  // expect: scada-banned
}

void numbers(const char* text) {
    int value = std::atoi(text);  // expect: scada-banned
    std::atof(text);  // expect: scada-banned
    std::strtol(text, nullptr, 10);  // expect: scada-banned
    std::sscanf(text, "%d", &value);  // expect: scada-banned
    std::scanf("%d", &value);  // expect: scada-banned
    std::srand(1);  // expect: scada-banned
    value = std::rand();  // expect: scada-banned
    value = rand();  // expect: scada-banned
}

void casts(const int* input) {
    const long wide = (long)*input;  // expect: scada-banned
    int* writable = const_cast<int*>(input);  // expect: scada-banned
    auto* bytes = reinterpret_cast<char*>(writable);  // expect: scada-banned
    (void)wide;  // Allowed: a cast to void discards a value.
    (void)bytes;
}

void processes() {
    std::system("true");  // expect: scada-banned
    popen("true", "r");  // expect: scada-banned
}

void threads() {
    std::thread worker([] {});
    worker.detach();  // expect: scada-banned
    guard.lock();  // expect: scada-banned
    guard.unlock();  // expect: scada-banned
    if (guard.try_lock()) {  // expect: scada-banned
        const std::scoped_lock allowed(guard);
    }
}

int jumps() {
    std::jmp_buf state;
    if (setjmp(state) != 0) {  // expect: scada-banned
        goto done;  // expect: scada-banned
    }
    std::longjmp(state, 1);  // expect: scada-banned
done:
    return 0;
}

// A function pointer to a banned function is a use of it.
using Copy = char* (*)(char*, const char*);
const Copy COPY = &std::strcpy;  // expect: scada-banned

}  // namespace fixture
