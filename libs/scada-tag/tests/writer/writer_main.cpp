// scada-tag-test-writer: breaks contract 13.2 on purpose. It attaches as a
// process, writes one UINT64 tag of its own from the main thread and then
// from a second thread, prints "second-thread written" and exits with code
// 0. A library built without NDEBUG stops the process at the second write
// instead. Test code; not installed. Writing at a rate is `scada-sim`'s job
// (issue P1-10).
//
// Usage: scada-tag-test-writer <base directory> <process> <tag>
//
// Exit code 1: an error; 2: wrong arguments.

#include "scada/tag/scada_tag.h"

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t ARGUMENTS = 4;

scada_value value_of(std::uint64_t number) noexcept {
    scada_value value{};
    value.type = SCADA_TYPE_UINT64;
    value.quality = SCADA_QUALITY_GOOD;
    value.value = number;
    return value;
}

int run(std::span<const std::string_view> args) {
    const std::string base(args[1]);
    const std::string process(args[2]);
    const std::string tag(args[3]);
    const scada_attach_opts options{.base_dir = base.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    scada_ctx* ctx = nullptr;
    const int attached = scada_tag_attach_opts(process.c_str(), &options, &ctx);
    if (attached != SCADA_OK) {
        std::cerr << "scada-tag-test-writer: attach failed with code "
                  << attached << "\n";
        return EXIT_FAILED;
    }
    std::uint32_t handle = SCADA_INVALID_HANDLE;
    if (scada_tag_resolve(ctx, tag.c_str(), &handle) != SCADA_OK) {
        std::cerr << "scada-tag-test-writer: no tag " << tag << "\n";
        scada_tag_detach(ctx);
        return EXIT_FAILED;
    }
    const scada_value first = value_of(1);
    const int first_code = scada_tag_write(ctx, handle, &first);
    int second_code = SCADA_E_INVALID;
    {
        // Joined at the end of the block, before `second_code` is read.
        const std::jthread other([ctx, handle, &second_code] {
            const scada_value second = value_of(2);
            second_code = scada_tag_write(ctx, handle, &second);
        });
    }
    scada_tag_detach(ctx);
    if (first_code != SCADA_OK || second_code != SCADA_OK) {
        std::cerr << "scada-tag-test-writer: a write failed\n";
        return EXIT_FAILED;
    }
    std::cout << "second-thread written\n" << std::flush;
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() != ARGUMENTS) {
        std::cerr << "scada-tag-test-writer: expected 3 arguments\n";
        return EXIT_USAGE;
    }
    std::array<std::string_view, ARGUMENTS> args;
    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        args.at(i) = raw_args[i];
    }
    return run(args);
}
