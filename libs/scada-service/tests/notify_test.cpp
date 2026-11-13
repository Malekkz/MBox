// Unit test for libscada-service. It uses no test framework: the framework is
// chosen in issue P0-03, and this file moves to it then.

#include "scada/service/notify.h"
#include "scada/service/notify_listener.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using scada::service::NotifyEnvironment;
using scada::service::Notifier;
using scada::service::NotifyListener;
using scada::service::NotifySettings;
using scada::service::NotifyStatus;
using scada::service::parse_notify_environment;

constexpr std::int64_t OWN_PID = 4242;
constexpr std::chrono::milliseconds RECEIVE_TIMEOUT{5000};

// Counts failed expectations and reports each one.
class Checker {
public:
    void expect(bool condition, std::string_view what) {
        if (!condition) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures_;
        }
    }

    [[nodiscard]] bool passed() const { return failures_ == 0; }

private:
    int failures_ = 0;
};

NotifyEnvironment environment(std::optional<std::string> socket,
                              std::optional<std::string> usec = std::nullopt,
                              std::optional<std::string> pid = std::nullopt) {
    return {std::move(socket), std::move(usec), std::move(pid)};
}

void test_parse_valid(Checker& checker) {
    {
        const auto settings = parse_notify_environment({}, OWN_PID);
        checker.expect(settings && settings->socket_name.empty() &&
                           !settings->watchdog_interval,
                       "nothing set: valid, disabled, no watchdog");
    }
    {
        const auto settings =
            parse_notify_environment(environment(""), OWN_PID);
        checker.expect(settings && settings->socket_name.empty(),
                       "empty socket name: disabled");
    }
    {
        const auto settings = parse_notify_environment(
            environment("/run/systemd/notify", "30000000", "4242"), OWN_PID);
        checker.expect(settings && settings->socket_name == "/run/systemd/notify",
                       "path socket: accepted");
        checker.expect(settings && settings->watchdog_interval ==
                                       std::chrono::seconds(30),
                       "watchdog for this process: interval set");
    }
    {
        const auto settings = parse_notify_environment(
            environment("@abstract", "1"), OWN_PID);
        checker.expect(settings && settings->socket_name == "@abstract",
                       "abstract socket: accepted");
        checker.expect(settings && settings->watchdog_interval ==
                                       std::chrono::microseconds(1),
                       "no WATCHDOG_PID: watchdog applies");
    }
    {
        const auto settings = parse_notify_environment(
            environment("@abstract", "30000000", "1"), OWN_PID);
        checker.expect(settings && !settings->watchdog_interval,
                       "watchdog for another process: no interval");
    }
    {
        const auto settings = parse_notify_environment(
            environment("@abstract", "86400000000"), OWN_PID);
        checker.expect(settings && settings->watchdog_interval ==
                                       scada::service::MAX_WATCHDOG_INTERVAL,
                       "largest interval: accepted");
    }
    {
        // 107 bytes is the longest name a socket address holds.
        const auto settings = parse_notify_environment(
            environment("/" + std::string(106, 'a')), OWN_PID);
        checker.expect(settings.has_value(), "longest socket name: accepted");
    }
}

void test_parse_invalid(Checker& checker) {
    const auto rejected = [&checker](const NotifyEnvironment& env,
                                     std::string_view what) {
        checker.expect(!parse_notify_environment(env, OWN_PID), what);
    };
    rejected(environment("relative/path"), "relative socket path");
    rejected(environment("/"), "socket name of one character");
    rejected(environment("@"), "abstract socket without a name");
    rejected(environment("/" + std::string(107, 'a')), "oversized socket name");
    rejected(environment("/" + std::string(100000, 'a')),
             "very long socket name");
    rejected(environment(std::string("/a\0b", 4)), "socket name with NUL");
    rejected(environment("@s", ""), "empty interval");
    rejected(environment("@s", "0"), "zero interval");
    rejected(environment("@s", "-5"), "negative interval");
    rejected(environment("@s", "+5"), "interval with a sign");
    rejected(environment("@s", " 5"), "interval with a space");
    rejected(environment("@s", "5s"), "interval with a unit");
    rejected(environment("@s", "0x10"), "hexadecimal interval");
    rejected(environment("@s", "86400000001"), "interval above the maximum");
    rejected(environment("@s", "99999999999999999999999999"),
             "interval that overflows");
    rejected(environment("@s", "5", "abc"), "non-numeric process id");
    rejected(environment("@s", "5", "0"), "process id zero");
    rejected(environment("@s", std::nullopt, ""), "empty process id");
    rejected(environment(std::nullopt, "abc"),
             "invalid interval without a socket");
}

void test_disabled(Checker& checker) {
    Notifier notifier(NotifySettings{});
    checker.expect(!notifier.enabled(), "disabled: not enabled");
    checker.expect(notifier.ready() == NotifyStatus::DISABLED,
                   "disabled: ready is not sent");
    checker.expect(notifier.watchdog() == NotifyStatus::DISABLED,
                   "disabled: watchdog is not sent");
}

void test_send(Checker& checker) {
    std::optional<NotifyListener> listener = NotifyListener::create();
    checker.expect(listener.has_value(), "listener: created");
    if (!listener) {
        return;
    }
    const auto settings = parse_notify_environment(
        environment(listener->socket_name()), OWN_PID);
    checker.expect(settings.has_value(), "listener name: valid");
    if (!settings) {
        return;
    }
    Notifier notifier(*settings);
    checker.expect(notifier.enabled(), "send: enabled");

    checker.expect(notifier.ready() == NotifyStatus::SENT, "ready: sent");
    checker.expect(listener->receive(RECEIVE_TIMEOUT) == "READY=1",
                   "ready: received");
    checker.expect(notifier.watchdog() == NotifyStatus::SENT, "watchdog: sent");
    checker.expect(listener->receive(RECEIVE_TIMEOUT) == "WATCHDOG=1",
                   "watchdog: received");
    checker.expect(notifier.stopping() == NotifyStatus::SENT, "stopping: sent");
    checker.expect(listener->receive(RECEIVE_TIMEOUT) == "STOPPING=1",
                   "stopping: received");
    checker.expect(notifier.status("all well") == NotifyStatus::SENT,
                   "status: sent");
    checker.expect(listener->receive(RECEIVE_TIMEOUT) == "STATUS=all well",
                   "status: received");

    const std::string longest(scada::service::MAX_NOTIFY_MESSAGE_SIZE, 'x');
    checker.expect(notifier.send(longest) == NotifyStatus::SENT,
                   "longest message: sent");
    checker.expect(listener->receive(RECEIVE_TIMEOUT) == longest,
                   "longest message: received whole");

    checker.expect(notifier.send("") == NotifyStatus::INVALID_MESSAGE,
                   "empty message: rejected");
    checker.expect(notifier.send(longest + "x") ==
                       NotifyStatus::INVALID_MESSAGE,
                   "oversized message: rejected");
    checker.expect(notifier.send(std::string_view("A=1\0B", 5)) ==
                       NotifyStatus::INVALID_MESSAGE,
                   "message with NUL: rejected");
    checker.expect(notifier.status("two\nlines") ==
                       NotifyStatus::INVALID_MESSAGE,
                   "status with a line break: rejected");
    checker.expect(!listener->receive(std::chrono::milliseconds(0)),
                   "rejected messages: nothing was sent");
}

void test_send_without_receiver(Checker& checker) {
    std::string name;
    {
        std::optional<NotifyListener> listener = NotifyListener::create();
        checker.expect(listener.has_value(), "second listener: created");
        if (!listener) {
            return;
        }
        name = listener->socket_name();
    }
    // The listener is gone, so nothing is bound to the name any more.
    Notifier notifier(NotifySettings{name, std::nullopt});
    checker.expect(notifier.ready() == NotifyStatus::FAILED,
                   "no receiver: send fails");
}

}  // namespace

int main() {
    Checker checker;
    test_parse_valid(checker);
    test_parse_invalid(checker);
    test_disabled(checker);
    test_send(checker);
    test_send_without_receiver(checker);
    return checker.passed() ? 0 : 1;
}
