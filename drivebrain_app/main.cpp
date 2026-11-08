#include <FoxgloveServer.hpp>
#include <MCAPLogger.hpp>
#include <drivebrain_msgs.pb.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <fmt/chrono.h>
#include <spdlog/spdlog.h>
#include <FlowmeterComms.hpp>

std::atomic<bool> running{true};

void sig_handler(int signal) {
    if(signal == SIGINT || signal == SIGTERM) {
      spdlog::warn("Interrupted, stopping mini-drivebrain");
      running = false;
    }
}

int main() {
  std::signal(SIGINT, sig_handler);
  std::signal(SIGTERM, sig_handler);

  core::MCAPLogger::create(mcap::McapWriterOptions(""));
  core::FoxgloveServer::create();

  core::MCAPLogger::instance().open_new_mcap();
  core::MCAPLogger::instance().init_logging();

  spdlog::info("Constructed logging singletons");

  // ADDED: create the flowmeter AFTER the logging singletons exist,
  // because its consumer thread calls the logger.
  constexpr const char* kGpioChip = "/dev/gpiochip0";
  constexpr std::uint32_t kGpioLine = 12;
  std::unique_ptr<comms::FlowmeterComms> flowmeter;
  try {
    flowmeter = std::make_unique<comms::FlowmeterComms>(kGpioChip, kGpioLine);
  } catch (const std::exception& e) {
    spdlog::error("Failed to start flowmeter: {}", e.what());
    core::MCAPLogger::instance().destroy();
    core::FoxgloveServer::instance().destroy();
    return 1;
  }
  std::uint64_t last_count = 0;

  auto loop_time = 0.004;
  std::chrono::microseconds loop_time_ms((int) (loop_time * 1000000.0f));
  auto next_tick = std::chrono::steady_clock::now();

  while(running) {

    next_tick += loop_time_ms;

    auto now = std::chrono::steady_clock::now();
    if(now > next_tick) {
      spdlog::warn("Loop overrun by {}", now-next_tick);
      next_tick = now;
    }

    // ADDED: read the latest count (atomic, so this is thread-safe) and log it.
    std::uint64_t count = flowmeter->get_pulse_count();
    if (count != last_count) {
      spdlog::info("Pulse count: {}", count);
      last_count = count;
    }

    std::this_thread::sleep_until(next_tick);
  }

  // ADDED: stop and join the flowmeter threads BEFORE the logger is destroyed,
  // so the consumer can't log into a dead logger and the MCAP closes cleanly.
  flowmeter.reset();

  core::MCAPLogger::instance().destroy();
  core::FoxgloveServer::instance().destroy();
  spdlog::info("Logging singletons safely destroyed");

  return 0;
}
