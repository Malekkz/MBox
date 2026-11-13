// Stand-in for the fuzzing engine in builds without libFuzzer: feeds every
// file named on the command line, or found directly in a named directory, to
// the fuzz target once. This makes the seed corpus a regression test on every
// compiler, architecture and sanitizer.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <system_error>
#include <vector>

// Defined by the fuzz target this file is linked with.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

namespace {

bool replay(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        std::cerr << "cannot read " << file << '\n';
        return false;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),
                                  std::istreambuf_iterator<char>()};
    const std::vector<std::uint8_t> input(bytes.begin(), bytes.end());
    LLVMFuzzerTestOneInput(input.data(), input.size());
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::span<char*> args(argv,
                                argc > 0 ? static_cast<std::size_t>(argc) : 0);
    std::size_t replayed = 0;
    for (const char* arg : args.subspan(args.empty() ? 0 : 1)) {
        const std::filesystem::path path(arg);
        std::error_code error;
        if (!std::filesystem::is_directory(path, error)) {
            if (!replay(path)) {
                return 1;
            }
            ++replayed;
            continue;
        }
        for (std::filesystem::directory_iterator it(path, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) {
                if (!replay(it->path())) {
                    return 1;
                }
                ++replayed;
            }
        }
        if (error) {
            std::cerr << "cannot list " << path << ": " << error.message()
                      << '\n';
            return 1;
        }
    }
    if (replayed == 0) {
        // An empty or misnamed corpus must not look like a passing test.
        std::cerr << "no inputs to replay\n";
        return 1;
    }
    std::cout << "replayed " << replayed << " inputs\n";
    return 0;
}
