// scada-tag-test-queue: one end of a reliable queue in the integration
// tests (contract, section 9). It attaches as one process through the C
// interface, does what its mode says, and then writes its heartbeat until
// it is stopped or killed. Test code; not installed.
//
// Usage: scada-tag-test-queue <base directory> <process> <mode> <queue>
//            [count]
//
// Modes:
//   post <n>     posts n event records whose time_ns is their number and
//                whose payload is derived from it (`payload_of`), retrying
//                on SCADA_E_FULL and SCADA_E_PEER_DOWN; prints "posted <n>".
//   consume <n>  takes and acknowledges n records, printing "got <seq>" for
//                each.
//   hold         takes one record without acknowledging it and prints
//                "holding <seq>"; the test kills it in that state.
//   drive        a driver: takes the requests of a cmd queue, prints "took
//                <request_id>" for each and answers OK in the result queue
//                of the same name after the prefix.
//   drive-hold   takes one request, prints "took <request_id>" and answers
//                nothing; the test kills it in that state.
//
// The uid of scada-tagd is taken to be the user of the caller, as in the
// tests. After attach and open it prints "attached aborted=<n>
// skipped=<n>" with the counts of scada_queue_stats for the queue. On SIGTERM
// it closes the queues, detaches and exits with code 0. Exit code 1: an error;
// 2: wrong arguments.

#include "scada/tag/scada_tag.h"

#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t MIN_ARGUMENTS = 5;
constexpr std::size_t MAX_ARGUMENTS = 6;
constexpr std::uint64_t MAX_COUNT = 100'000'000;
// How long one wait for a record lasts, so that the heartbeat keeps going.
constexpr int WAIT_MS = 50;
constexpr long WAIT_NS = 50'000'000;
// How often a producer calls scada_tag_notify between two waits.
constexpr std::uint64_t NOTIFY_EVERY = 64;
// The pause of a producer whose queue is full, in ns.
constexpr long BACKOFF_NS = 100'000;
constexpr std::uint64_t PAYLOAD_FACTOR = 0x9E37'79B9'7F4A'7C15U;
constexpr std::size_t PAYLOAD_BYTES = 8;
constexpr unsigned BITS_PER_BYTE = 8;

// The whole of `text` as a decimal number of at most `limit`.
std::optional<std::uint64_t> parse_number(std::string_view text,
                                          std::uint64_t limit) {
    std::uint64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value > limit) {
        return std::nullopt;
    }
    return value;
}

// The 8 payload bytes of event `number`: what the test checks.
std::uint64_t payload_of(std::uint64_t number) noexcept {
    return number * PAYLOAD_FACTOR;
}

class Helper {
public:
    Helper(scada_ctx* context, const sigset_t& signals)
        : context_(context), signals_(signals) {}

    // True once SIGTERM has arrived. Writes the heartbeat.
    bool stopping() {
        static_cast<void>(scada_tag_heartbeat(context_));
        const timespec none{};
        if (!stopped_ && ::sigtimedwait(&signals_, nullptr, &none) >= 0) {
            stopped_ = true;
        }
        return stopped_;
    }

    int post(scada_queue* queue, std::uint64_t count) {
        for (std::uint64_t i = 0; i < count;) {
            scada_event_record record{};
            record.time_ns = static_cast<std::int64_t>(i);
            record.kind = SCADA_EVENT_SYSTEM;
            record.payload_len = PAYLOAD_BYTES;
            record.source_handle = SCADA_INVALID_HANDLE;
            const std::uint64_t payload = payload_of(i);
            for (std::size_t byte = 0; byte < PAYLOAD_BYTES; ++byte) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): byte is below PAYLOAD_BYTES, inside the array
                record.payload[byte] = static_cast<std::uint8_t>(
                    payload >> (byte * BITS_PER_BYTE));
            }
            const int code = scada_queue_post(queue, &record, sizeof(record));
            if (code == SCADA_OK) {
                ++i;
                if (i % NOTIFY_EVERY == 0) {
                    static_cast<void>(scada_tag_notify(context_));
                    static_cast<void>(scada_tag_heartbeat(context_));
                }
                continue;
            }
            if (code != SCADA_E_FULL && code != SCADA_E_PEER_DOWN) {
                std::cerr << "post failed with code " << code << "\n";
                return EXIT_FAILED;
            }
            static_cast<void>(scada_tag_notify(context_));
            if (stopping()) {
                return 0;
            }
            const timespec pause{.tv_sec = 0, .tv_nsec = BACKOFF_NS};
            static_cast<void>(::nanosleep(&pause, nullptr));
        }
        static_cast<void>(scada_tag_notify(context_));
        std::cout << "posted " << count << "\n" << std::flush;
        return 0;
    }

    // Takes `count` records, or one if `hold`, then returns.
    int consume(scada_queue* queue, std::uint64_t count, bool hold) {
        for (std::uint64_t i = 0; i < count;) {
            scada_event_record record{};
            const int code =
                scada_queue_next(queue, &record, sizeof(record), WAIT_MS);
            if (code == SCADA_OK) {
                if (hold) {
                    std::cout << "holding " << record.seq << "\n" << std::flush;
                    return 0;
                }
                if (scada_queue_ack(queue) != SCADA_OK) {
                    return EXIT_FAILED;
                }
                std::cout << "got " << record.seq << "\n" << std::flush;
                ++i;
            } else if (code != SCADA_E_TIMEOUT) {
                std::cerr << "next failed with code " << code << "\n";
                return EXIT_FAILED;
            }
            if (stopping()) {
                return 0;
            }
        }
        return 0;
    }

    // Answers requests until SIGTERM; with `hold`, takes one and returns.
    int drive(scada_queue* requests, scada_queue* results, bool hold) {
        for (;;) {
            scada_write_request request{};
            const int code =
                scada_queue_next(requests, &request, sizeof(request), WAIT_MS);
            if (code == SCADA_OK) {
                std::cout << "took " << request.request_id << "\n"
                          << std::flush;
                if (hold) {
                    return 0;
                }
                scada_write_result result{};
                result.request_id = request.request_id;
                result.status = SCADA_RESULT_OK;
                if (scada_queue_post(results, &result, sizeof(result)) !=
                    SCADA_OK) {
                    std::cerr << "the answer could not be posted\n";
                }
                static_cast<void>(scada_tag_notify(context_));
            } else if (code != SCADA_E_TIMEOUT) {
                std::cerr << "next failed with code " << code << "\n";
                return EXIT_FAILED;
            }
            if (stopping()) {
                return 0;
            }
        }
    }

    // Writes the heartbeat until SIGTERM.
    void idle() {
        const timespec period{.tv_sec = 0, .tv_nsec = WAIT_NS};
        while (!stopping()) {
            static_cast<void>(::nanosleep(&period, nullptr));
        }
    }

private:
    scada_ctx* context_;
    sigset_t signals_;
    bool stopped_ = false;  // SIGTERM has arrived
};

int run(std::span<const std::string_view> args) {
    const std::string base(args[1]);
    const std::string process(args[2]);
    const std::string_view mode = args[3];
    const std::string queue_name(args[4]);
    std::uint64_t count = 1;
    const bool counted = mode == "post" || mode == "consume";
    if (counted) {
        const std::optional<std::uint64_t> parsed =
            args.size() == MAX_ARGUMENTS ? parse_number(args[5], MAX_COUNT)
                                         : std::nullopt;
        if (!parsed.has_value()) {
            std::cerr << "scada-tag-test-queue: bad count\n";
            return EXIT_USAGE;
        }
        count = *parsed;
    } else if (args.size() != MIN_ARGUMENTS ||
               (mode != "hold" && mode != "drive" && mode != "drive-hold")) {
        std::cerr << "scada-tag-test-queue: bad argument\n";
        return EXIT_USAGE;
    }

    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "scada-tag-test-queue: cannot block signals\n";
        return EXIT_FAILED;
    }
    const scada_attach_opts options{.base_dir = base.c_str(),
                                    .retain_dir = nullptr,
                                    .tagd_uid = std::int64_t{::geteuid()}};
    scada_ctx* context = nullptr;
    if (const int code =
            scada_tag_attach_opts(process.c_str(), &options, &context);
        code != SCADA_OK) {
        std::cerr << "scada-tag-test-queue: attach failed with code " << code
                  << "\n";
        return EXIT_FAILED;
    }
    scada_queue* queue = nullptr;
    scada_queue* results = nullptr;
    int code = scada_queue_open(context, queue_name.c_str(), &queue);
    const bool driver = mode == "drive" || mode == "drive-hold";
    if (code == SCADA_OK && driver) {
        const std::string result_name =
            "result." + queue_name.substr(std::string_view("cmd.").size());
        code = scada_queue_open(context, result_name.c_str(), &results);
    }
    if (code != SCADA_OK) {
        std::cerr << "scada-tag-test-queue: open failed with code " << code
                  << "\n";
        scada_tag_detach(context);
        return EXIT_FAILED;
    }
    scada_queue_stats_t stats{};
    static_cast<void>(scada_queue_stats(queue, &stats));
    std::cout << "attached aborted=" << stats.aborted
              << " skipped=" << stats.skipped << "\n"
              << std::flush;

    Helper helper(context, signals);
    int exit_code = 0;
    if (mode == "post") {
        exit_code = helper.post(queue, count);
    } else if (driver) {
        exit_code = helper.drive(queue, results, mode == "drive-hold");
    } else {
        exit_code = helper.consume(queue, count, mode == "hold");
    }
    if (exit_code == 0) {
        helper.idle();
    }
    scada_queue_close(results);
    scada_queue_close(queue);
    scada_tag_detach(context);
    return exit_code;
}

}  // namespace

int main(int argc, char* argv[]) {
    // argv is the one C array the language hands us; wrap it with its length
    // at once so that nothing below indexes a raw pointer.
    const std::span<char*> raw_args(
        argv, argc > 0 ? static_cast<std::size_t>(argc) : 0);
    if (raw_args.size() < MIN_ARGUMENTS || raw_args.size() > MAX_ARGUMENTS) {
        std::cerr << "scada-tag-test-queue: expected 4 or 5 arguments\n";
        return EXIT_USAGE;
    }
    std::array<std::string_view, MAX_ARGUMENTS> args;
    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        args.at(i) = raw_args[i];
    }
    return run(std::span<const std::string_view>(args).first(raw_args.size()));
}
