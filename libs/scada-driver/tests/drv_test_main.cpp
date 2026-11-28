// scada-drv-test: a driver process of the test protocol (driver interface
// contract 15: "a scripted frame protocol over a loopback transport"), for
// the integration tests of the driver process (decision D-142). The
// configuration is given on the command line until the framework's compile
// step (slice 8); `--help` prints it. The logic is in drv_test_support:
// `main` only parses, wires and runs.

#include "drv_test_support.h"
#include "scada/common/log.h"
#include "scada/common/result.h"
#include "scada/driver/compiled.h"
#include "scada/driver/connections.h"
#include "scada/driver/driver.h"
#include "scada/driver/transport.h"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    namespace driver = scada::driver;
    // First, before the logger's thread (driver.h).
    if (!driver::block_stop_signals().ok()) {
        return driver::EXIT_ERROR;
    }
    std::optional<driver::test::DrvTestOptions> options =
        driver::test::parse_drv_test_args(
            std::span<const char* const>(argv, static_cast<std::size_t>(argc)),
            std::cerr);
    if (!options) {
        return driver::EXIT_USAGE;
    }
    if (options->help) {
        std::cout << driver::test::drv_test_usage();
        return driver::EXIT_OK;
    }
    const driver::test::ThreadCheckedProtocol protocol(options->config);
    if (!driver::test::plan_all(options->config, protocol, std::cerr)) {
        return driver::EXIT_USAGE;
    }
    scada::Result<std::unique_ptr<scada::LogSink>> sink =
        scada::make_default_sink();
    if (!sink.ok()) {
        return driver::EXIT_ERROR;
    }
    std::unique_ptr<scada::LogSink> output = std::move(sink).value();
    if (options->refuse_log) {
        output = driver::test::refusing_sink(std::move(output),
                                              options->refuse_log_ms);
    }
    scada::Result<scada::Logger> logger = scada::Logger::create(
        std::move(output), {.component = "scada-drv-test"});
    if (!logger.ok()) {
        return driver::EXIT_ERROR;
    }
    const driver::CompiledConfig& config = options->config;
    const std::vector<driver::test::Endpoint>& endpoints = options->endpoints;
    auto transports = [&](const driver::CompiledChannel& channel,
                          const driver::Clock& clock) {
        const auto index =
            static_cast<std::size_t>(&channel - config.channels.data());
        return driver::test::make_transport(channel, endpoints.at(index),
                                            clock);
    };
    driver::SystemClock clock;
    driver::Driver process(config, protocol, transports, clock,
                           options->settings, &logger.value());
    const driver::RunEnd end = process.run();
    if (end == driver::RunEnd::FAILED) {
        std::cerr << "scada-drv-test: " << process.failure_detail() << "\n";
    }
    if (!process.joined()) {
        // A thread still runs and cannot be joined (coding standard 7): the
        // log is flushed, and the process ends without destroying anything.
        constexpr std::chrono::milliseconds FLUSH_LIMIT{1000};
        // A flush that times out loses only records: the exit goes on.
        (void)logger.value().flush(FLUSH_LIMIT);
        std::_Exit(driver::exit_status(end));
    }
    return driver::exit_status(end);
}
