// scada-tag-test-crosser: one end of a queue across generation changes, for
// the queue part of conformance test 21 (contract, sections 9.5 and 11.3;
// decision D-54). It attaches through the C interface and, whenever a queue
// call returns SCADA_E_RETIRING, closes its queues, detaches, attaches again
// to `current` and goes on. Test code; not installed.
//
// Usage: scada-tag-test-crosser <base directory> <process> <mode> <queue>
//            [<second queue> | <count>]
//
// Modes:
//   post <events> <n>      posts the events 0 .. n-1 (time_ns is the
//                          number), one about every millisecond, and
//                          prints "ok <number>" for each that returned
//                          SCADA_OK; a number refused with SCADA_E_RETIRING
//                          is posted again after the reattach. Prints
//                          "done" at the end and then idles.
//   consume <events>       consumes and acknowledges events, printing
//                          "got <time_ns>" for each.
//   request <cmd> <n>      posts n write requests with a lifetime of 10 s,
//                          one about every 5 ms, printing "posted <stamp>
//                          gen <generation>" for each that returned
//                          SCADA_OK, where <stamp> is its expiry_mono_ns,
//                          unique to the request;
//                          prints "done" at the end and then idles.
//   drive <cmd> <result>   takes requests, prints "took <stamp> gen
//                          <generation>" for each, and answers OK.
//
// Every process prints "attached gen <generation>" at each attach. The uid
// of scada-tagd is taken to be the user of the caller. On SIGTERM it
// detaches and exits with code 0. Exit code 1: an error; 2: wrong arguments.

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
#include <utility>

namespace {

constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr std::size_t MIN_ARGUMENTS = 5;
constexpr std::size_t MAX_ARGUMENTS = 6;
// Positions of the arguments after the program name.
constexpr std::size_t ARG_BASE = 1;
constexpr std::size_t ARG_PROCESS = 2;
constexpr std::size_t ARG_MODE = 3;
constexpr std::size_t ARG_QUEUE = 4;
constexpr std::size_t ARG_LAST = 5;
constexpr std::uint64_t MAX_COUNT = 1'000'000;
constexpr int WAIT_MS = 20;
constexpr long POST_PAUSE_NS = 1'000'000;
constexpr long REQUEST_PAUSE_NS = 5'000'000;
constexpr long RETRY_PAUSE_NS = 10'000'000;
constexpr std::int64_t REQUEST_LIFETIME_NS = 10'000'000'000;
constexpr std::int64_t NANOSECONDS_PER_SECOND = 1'000'000'000;
// Attach attempts after a RETIRING: scada-tagd may still be building the
// next generation.
constexpr int ATTACH_ATTEMPTS = 1000;

std::int64_t monotonic_now() noexcept {
    timespec now{};
    static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
    return (std::int64_t{now.tv_sec} * NANOSECONDS_PER_SECOND) + now.tv_nsec;
}

// A pause, not a wait for a condition (review of PR #82, N3): the posts are
// paced on purpose, so that the generation change of the test falls into
// the middle of the stream, and an attach after RETIRING has nothing to
// wait on while scada-tagd builds the next generation; it is retried.
void pause_ns(long nanoseconds) noexcept {
    const timespec pause{.tv_sec = 0, .tv_nsec = nanoseconds};
    static_cast<void>(::nanosleep(&pause, nullptr));
}

std::optional<std::uint64_t> parse_number(std::string_view text) {
    std::uint64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value > MAX_COUNT) {
        return std::nullopt;
    }
    return value;
}

// The process, attached to one generation at a time, with up to two
// queues.
class Crosser {
public:
    Crosser(std::string base, std::string process, std::string first,
            std::string second, const sigset_t& signals)
        : base_(std::move(base)),
          process_(std::move(process)),
          first_name_(std::move(first)),
          second_name_(std::move(second)),
          signals_(signals) {}
    Crosser(const Crosser&) = delete;
    Crosser& operator=(const Crosser&) = delete;
    Crosser(Crosser&&) = delete;
    Crosser& operator=(Crosser&&) = delete;
    ~Crosser() { detach(); }

    // Attaches to `current` and opens the queues. False on an error.
    [[nodiscard]] bool attach() {
        const scada_attach_opts options{.base_dir = base_.c_str(),
                                        .retain_dir = nullptr,
                                        .tagd_uid = std::int64_t{::geteuid()}};
        for (int attempt = 0; attempt < ATTACH_ATTEMPTS; ++attempt) {
            if (scada_tag_attach_opts(process_.c_str(), &options, &ctx_) ==
                SCADA_OK) {
                break;
            }
            ctx_ = nullptr;
            pause_ns(RETRY_PAUSE_NS);
        }
        if (ctx_ == nullptr ||
            scada_queue_open(ctx_, first_name_.c_str(), &first_) != SCADA_OK ||
            (!second_name_.empty() &&
             scada_queue_open(ctx_, second_name_.c_str(), &second_) !=
                 SCADA_OK)) {
            std::cerr << "scada-tag-test-crosser: cannot attach and open\n";
            return false;
        }
        scada_status status{};
        static_cast<void>(scada_tag_status(ctx_, &status));
        generation_ = status.generation;
        std::cout << "attached gen " << generation_ << "\n" << std::flush;
        return true;
    }

    // After SCADA_E_RETIRING: detach and attach to `current`.
    [[nodiscard]] bool reattach() {
        detach();
        return attach();
    }

    // True once SIGTERM has arrived. Writes the heartbeat, and reattaches
    // when the generation retires.
    [[nodiscard]] bool stopping() {
        if (scada_tag_heartbeat(ctx_) == SCADA_E_RETIRING && !reattach()) {
            return true;
        }
        const timespec none{};
        return ::sigtimedwait(&signals_, nullptr, &none) >= 0;
    }

    int post(std::uint64_t count) {
        for (std::uint64_t number = 0; number < count;) {
            scada_event_record record{};
            record.time_ns = static_cast<std::int64_t>(number);
            record.kind = SCADA_EVENT_SYSTEM;
            record.source_handle = SCADA_INVALID_HANDLE;
            const int code = scada_queue_post(first_, &record, sizeof(record));
            if (code == SCADA_OK) {
                std::cout << "ok " << number << "\n" << std::flush;
                ++number;
                static_cast<void>(scada_tag_notify(ctx_));
            } else if (code == SCADA_E_RETIRING) {
                if (!reattach()) {
                    return EXIT_FAILED;
                }
                continue;
            } else if (code != SCADA_E_FULL && code != SCADA_E_PEER_DOWN) {
                std::cerr << "post failed with code " << code << "\n";
                return EXIT_FAILED;
            } else {
                // The consumer may have attached after this process asked
                // for its region: notify looks for it again.
                static_cast<void>(scada_tag_notify(ctx_));
            }
            if (stopping()) {
                return 0;
            }
            pause_ns(POST_PAUSE_NS);
        }
        std::cout << "done\n" << std::flush;
        idle();
        return 0;
    }

    int consume() {
        for (;;) {
            scada_event_record record{};
            const int code =
                scada_queue_next(first_, &record, sizeof(record), WAIT_MS);
            if (code == SCADA_OK) {
                if (scada_queue_ack(first_) != SCADA_OK) {
                    return EXIT_FAILED;
                }
                std::cout << "got " << record.time_ns << "\n" << std::flush;
                continue;
            }
            if (code == SCADA_E_RETIRING) {
                if (!reattach()) {
                    return EXIT_FAILED;
                }
                continue;
            }
            if (code != SCADA_E_TIMEOUT) {
                std::cerr << "next failed with code " << code << "\n";
                return EXIT_FAILED;
            }
            if (stopping()) {
                return 0;
            }
        }
    }

    int request(std::uint64_t count) {
        for (std::uint64_t posted = 0; posted < count;) {
            scada_write_request record{};
            record.target_handle = 0;
            record.operation = SCADA_OP_READ_NOW;
            record.source = SCADA_SOURCE_AUTO;
            record.expiry_mono_ns = monotonic_now() + REQUEST_LIFETIME_NS;
            const int code = scada_queue_post(first_, &record, sizeof(record));
            if (code == SCADA_OK) {
                std::cout << "posted " << record.expiry_mono_ns << " gen "
                          << generation_ << "\n"
                          << std::flush;
                ++posted;
                static_cast<void>(scada_tag_notify(ctx_));
            } else if (code == SCADA_E_RETIRING) {
                if (!reattach()) {
                    return EXIT_FAILED;
                }
                continue;
            } else if (code != SCADA_E_FULL && code != SCADA_E_PEER_DOWN) {
                std::cerr << "post failed with code " << code << "\n";
                return EXIT_FAILED;
            } else {
                // The consumer may have attached after this process asked
                // for its region: notify looks for it again.
                static_cast<void>(scada_tag_notify(ctx_));
            }
            if (stopping()) {
                return 0;
            }
            pause_ns(REQUEST_PAUSE_NS);
        }
        std::cout << "done\n" << std::flush;
        idle();
        return 0;
    }

    int drive() {
        for (;;) {
            scada_write_request request{};
            const int code =
                scada_queue_next(first_, &request, sizeof(request), WAIT_MS);
            if (code == SCADA_OK) {
                std::cout << "took " << request.expiry_mono_ns << " gen "
                          << generation_ << "\n"
                          << std::flush;
                scada_write_result result{};
                result.request_id = request.request_id;
                result.status = SCADA_RESULT_OK;
                static_cast<void>(
                    scada_queue_post(second_, &result, sizeof(result)));
                static_cast<void>(scada_tag_notify(ctx_));
                continue;
            }
            if (code == SCADA_E_RETIRING) {
                if (!reattach()) {
                    return EXIT_FAILED;
                }
                continue;
            }
            if (code != SCADA_E_TIMEOUT) {
                std::cerr << "next failed with code " << code << "\n";
                return EXIT_FAILED;
            }
            if (stopping()) {
                return 0;
            }
        }
    }

private:
    // Writes the heartbeat until SIGTERM, waiting for the signal itself,
    // at most RETRY_PAUSE_NS at a time.
    void idle() {
        const timespec period{.tv_sec = 0, .tv_nsec = RETRY_PAUSE_NS};
        for (;;) {
            if (scada_tag_heartbeat(ctx_) == SCADA_E_RETIRING && !reattach()) {
                return;
            }
            if (::sigtimedwait(&signals_, nullptr, &period) >= 0) {
                return;
            }
        }
    }

    void detach() {
        if (second_ != nullptr) {
            scada_queue_close(second_);
            second_ = nullptr;
        }
        if (first_ != nullptr) {
            scada_queue_close(first_);
            first_ = nullptr;
        }
        if (ctx_ != nullptr) {
            scada_tag_detach(ctx_);
            ctx_ = nullptr;
        }
    }

    std::string base_;
    std::string process_;
    std::string first_name_;
    std::string second_name_;
    sigset_t signals_;
    scada_ctx* ctx_ = nullptr;
    scada_queue* first_ = nullptr;
    scada_queue* second_ = nullptr;
    std::uint64_t generation_ = 0;
};

int run(std::span<const std::string_view> args) {
    const std::string_view mode = args[ARG_MODE];
    const bool counted = mode == "post" || mode == "request";
    std::optional<std::uint64_t> count;
    std::string second;
    if (counted && args.size() == MAX_ARGUMENTS) {
        count = parse_number(args[ARG_LAST]);
    } else if (mode == "drive" && args.size() == MAX_ARGUMENTS) {
        second = std::string(args[ARG_LAST]);
    } else if (mode != "consume" || args.size() != MIN_ARGUMENTS) {
        std::cerr << "scada-tag-test-crosser: bad arguments\n";
        return EXIT_USAGE;
    }
    if (counted && !count.has_value()) {
        std::cerr << "scada-tag-test-crosser: bad count\n";
        return EXIT_USAGE;
    }
    sigset_t signals{};
    if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGTERM) != 0 ||
        ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        return EXIT_FAILED;
    }
    Crosser crosser(std::string(args[ARG_BASE]), std::string(args[ARG_PROCESS]),
                    std::string(args[ARG_QUEUE]), second, signals);
    if (!crosser.attach()) {
        return EXIT_FAILED;
    }
    if (mode == "post") {
        return crosser.post(count.value_or(0));
    }
    if (mode == "request") {
        return crosser.request(count.value_or(0));
    }
    if (mode == "drive") {
        return crosser.drive();
    }
    return crosser.consume();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < static_cast<int>(MIN_ARGUMENTS) ||
        argc > static_cast<int>(MAX_ARGUMENTS)) {
        std::cerr << "usage: scada-tag-test-crosser <base> <process> <mode> "
                     "<queue> [<queue> | <count>]\n";
        return EXIT_USAGE;
    }
    std::array<std::string_view, MAX_ARGUMENTS> args{};
    const std::span<char*> given(argv, static_cast<std::size_t>(argc));
    for (std::size_t i = 0; i < given.size(); ++i) {
        args.at(i) = given[i];
    }
    return run(std::span(args).first(given.size()));
}
