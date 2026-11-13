// Unit tests for libscada-service.

#include "scada/service/notify.h"
#include "scada/service/notify_listener.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace {

using scada::service::Notifier;
using scada::service::NotifyEnvironment;
using scada::service::NotifyListener;
using scada::service::NotifySettings;
using scada::service::NotifyStatus;
using scada::service::parse_notify_environment;

constexpr std::int64_t OWN_PID = 4242;
constexpr std::chrono::milliseconds RECEIVE_TIMEOUT{5000};
constexpr std::chrono::seconds THIRTY_SECONDS{30};
// A socket address holds a name of at most 107 bytes.
constexpr std::size_t LONGEST_NAME = 107;
constexpr std::size_t VERY_LONG_NAME = 100000;

NotifyEnvironment environment(
    const std::optional<std::string>& socket,
    const std::optional<std::string>& usec = std::nullopt,
    const std::optional<std::string>& pid = std::nullopt) {
    NotifyEnvironment result;
    result.notify_socket = socket;
    result.watchdog_usec = usec;
    result.watchdog_pid = pid;
    return result;
}

// True if the environment is accepted.
bool accepted(const NotifyEnvironment& env) {
    return parse_notify_environment(env, OWN_PID).has_value();
}

TEST(NotifyEnvironmentParsing, NothingSetMeansNoServiceManager) {
    const auto settings = parse_notify_environment({}, OWN_PID);
    if (!settings.has_value()) {
        FAIL() << "an empty environment was rejected";
    }
    EXPECT_TRUE(settings->socket_name.empty());
    EXPECT_EQ(settings->watchdog_interval, std::nullopt);
}

TEST(NotifyEnvironmentParsing, EmptySocketNameMeansNoServiceManager) {
    const auto settings = parse_notify_environment(environment(""), OWN_PID);
    if (!settings.has_value()) {
        FAIL() << "an empty socket name was rejected";
    }
    EXPECT_TRUE(settings->socket_name.empty());
}

TEST(NotifyEnvironmentParsing, AcceptsAPathAndAWatchdogForThisProcess) {
    const auto settings = parse_notify_environment(
        environment("/run/systemd/notify", "30000000", "4242"), OWN_PID);
    if (!settings.has_value()) {
        FAIL() << "a valid environment was rejected";
    }
    EXPECT_EQ(settings->socket_name, "/run/systemd/notify");
    EXPECT_EQ(settings->watchdog_interval, THIRTY_SECONDS);
}

TEST(NotifyEnvironmentParsing, AcceptsAnAbstractNameWithoutWatchdogPid) {
    const auto settings =
        parse_notify_environment(environment("@abstract", "1"), OWN_PID);
    if (!settings.has_value()) {
        FAIL() << "a valid environment was rejected";
    }
    EXPECT_EQ(settings->socket_name, "@abstract");
    EXPECT_EQ(settings->watchdog_interval, std::chrono::microseconds(1));
}

TEST(NotifyEnvironmentParsing, IgnoresAWatchdogForAnotherProcess) {
    const auto settings = parse_notify_environment(
        environment("@abstract", "30000000", "1"), OWN_PID);
    if (!settings.has_value()) {
        FAIL() << "a valid environment was rejected";
    }
    EXPECT_EQ(settings->watchdog_interval, std::nullopt);
}

TEST(NotifyEnvironmentParsing, AcceptsTheLimits) {
    const auto settings = parse_notify_environment(
        environment("/" + std::string(LONGEST_NAME - 1, 'a'), "86400000000"),
        OWN_PID);
    if (!settings.has_value()) {
        FAIL() << "the longest name or the longest interval was rejected";
    }
    EXPECT_EQ(settings->watchdog_interval,
              scada::service::MAX_WATCHDOG_INTERVAL);
}

TEST(NotifyEnvironmentParsing, RejectsInvalidSocketNames) {
    EXPECT_FALSE(accepted(environment("relative/path")));
    EXPECT_FALSE(accepted(environment("/")));
    EXPECT_FALSE(accepted(environment("@")));
    EXPECT_FALSE(accepted(environment("/" + std::string(LONGEST_NAME, 'a'))));
    EXPECT_FALSE(accepted(environment("/" + std::string(VERY_LONG_NAME, 'a'))));
    EXPECT_FALSE(accepted(environment("/a" + std::string(1, '\0') + "b")));
}

TEST(NotifyEnvironmentParsing, RejectsInvalidIntervals) {
    // Empty, zero, signed, padded, with a unit, hexadecimal, above the
    // maximum, and too large for any integer.
    const std::array<std::string, 9> invalid{
        "",     "0",           "-5",
        "+5",   " 5",          "5s",
        "0x10", "86400000001", "99999999999999999999999999"};
    for (const std::string& interval : invalid) {
        EXPECT_FALSE(accepted(environment("@s", interval)))
            << "'" << interval << "'";
    }
    EXPECT_FALSE(accepted(environment(std::nullopt, "abc")));
}

TEST(NotifyEnvironmentParsing, RejectsInvalidProcessIds) {
    EXPECT_FALSE(accepted(environment("@s", "5", "abc")));
    EXPECT_FALSE(accepted(environment("@s", "5", "0")));
    EXPECT_FALSE(accepted(environment("@s", std::nullopt, "")));
}

TEST(Notifier, SendsNothingWithoutAServiceManager) {
    Notifier notifier(NotifySettings{});
    EXPECT_FALSE(notifier.enabled());
    EXPECT_EQ(notifier.ready(), NotifyStatus::DISABLED);
    EXPECT_EQ(notifier.watchdog(), NotifyStatus::DISABLED);
}

TEST(Notifier, SendsEachStateToTheListener) {
    std::optional<NotifyListener> listener = NotifyListener::create();
    if (!listener.has_value()) {
        FAIL() << "could not create a listener";
    }
    Notifier notifier(NotifySettings{listener->socket_name(), std::nullopt});

    EXPECT_EQ(notifier.ready(), NotifyStatus::SENT);
    EXPECT_EQ(listener->receive(RECEIVE_TIMEOUT), "READY=1");
    EXPECT_EQ(notifier.watchdog(), NotifyStatus::SENT);
    EXPECT_EQ(listener->receive(RECEIVE_TIMEOUT), "WATCHDOG=1");
    EXPECT_EQ(notifier.stopping(), NotifyStatus::SENT);
    EXPECT_EQ(listener->receive(RECEIVE_TIMEOUT), "STOPPING=1");
}

TEST(Notifier, SendsAStatusLineAndTheLongestMessage) {
    std::optional<NotifyListener> listener = NotifyListener::create();
    if (!listener.has_value()) {
        FAIL() << "could not create a listener";
    }
    Notifier notifier(NotifySettings{listener->socket_name(), std::nullopt});

    EXPECT_EQ(notifier.status("all well"), NotifyStatus::SENT);
    EXPECT_EQ(listener->receive(RECEIVE_TIMEOUT), "STATUS=all well");

    const std::string longest(scada::service::MAX_NOTIFY_MESSAGE_SIZE, 'x');
    EXPECT_EQ(notifier.send(longest), NotifyStatus::SENT);
    EXPECT_EQ(listener->receive(RECEIVE_TIMEOUT), longest);
}

TEST(Notifier, AcceptsTheSocketNameOfAListener) {
    std::optional<NotifyListener> listener = NotifyListener::create();
    if (!listener.has_value()) {
        FAIL() << "could not create a listener";
    }
    EXPECT_TRUE(accepted(environment(listener->socket_name())));
    const Notifier notifier(
        NotifySettings{listener->socket_name(), std::nullopt});
    EXPECT_TRUE(notifier.enabled());
}

TEST(Notifier, RejectsInvalidMessagesWithoutSending) {
    std::optional<NotifyListener> listener = NotifyListener::create();
    if (!listener.has_value()) {
        FAIL() << "could not create a listener";
    }
    Notifier notifier(NotifySettings{listener->socket_name(), std::nullopt});

    const std::string too_long(scada::service::MAX_NOTIFY_MESSAGE_SIZE + 1,
                               'x');
    EXPECT_EQ(notifier.send(""), NotifyStatus::INVALID_MESSAGE);
    EXPECT_EQ(notifier.send(too_long), NotifyStatus::INVALID_MESSAGE);
    EXPECT_EQ(notifier.send("A=1" + std::string(1, '\0') + "B"),
              NotifyStatus::INVALID_MESSAGE);
    EXPECT_EQ(notifier.status("two\nlines"), NotifyStatus::INVALID_MESSAGE);
    EXPECT_EQ(listener->receive(std::chrono::milliseconds(0)), std::nullopt);
}

TEST(Notifier, FailsWhenNothingListens) {
    std::string name;
    {
        std::optional<NotifyListener> listener = NotifyListener::create();
        if (!listener.has_value()) {
            FAIL() << "could not create a listener";
        }
        name = listener->socket_name();
    }
    // The listener is gone, so nothing is bound to the name any more.
    Notifier notifier(NotifySettings{name, std::nullopt});
    EXPECT_EQ(notifier.ready(), NotifyStatus::FAILED);
}

}  // namespace
