#pragma once
#include <linux/gpio.h>

#include <DriverBus.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace comms {
// FlowmeterComms: reads pulses from a flowmeter wired to a Raspberry Pi GPIO pin.
//
// Data flow:
//   GPIO pin --> reader thread --> pulse_queue_ --> processor thread --> pulse_count_
//                (producer)        (hand-off)        (consumer)          (read by main)
//
// RAII design: the constructor starts everything, and the destructor stops
// everything, so there is no separate start()/stop() to forget or call twice.
class FlowmeterComms {
  // Opens the GPIO chip (e.g. "/dev/gpiochip0"), requests one line on it
  // (gpio_line_offset = which pin on that chip) with rising-edge detection,
  // then starts both threads.
 public:
  // Constructor opens/configures the GPIO line and starts both threads.
  FlowmeterComms(const std::string& gpio_chip_path,
                 std::uint32_t gpio_line_offset);

  // Signals both threads to stop, wakes them, joins them (waits for them to
  // finish), then closes the file descriptors. Threads must be stopped first
  // so they never use a resource that has already been released.
  ~FlowmeterComms();

  // Owns threads and file descriptors, so copying is not allowed.
  FlowmeterComms(const FlowmeterComms&) = delete;
  FlowmeterComms& operator=(const FlowmeterComms&) = delete;

  // Called by main to read the latest value.
  std::uint64_t get_pulse_count() const;

 private:
  void read_pin_worker();      // producer: waits for pulses, pushes to queue
  void process_data_worker();  // consumer: waits on queue, handles pulses

  // File descriptors (integers the OS hands back). -1 means "not open yet".
  int chip_fd_ = -1;
  int line_fd_ = -1;

  // Shutdown flag. Both threads check it; setting it to false asks them to exit.
  // Set to true BEFORE starting the threads or they may exit immediately.
  std::atomic<bool> running_{false};

  // Instantiate the threadSafeDeque; we only count timestamps for this project, but
  // If we preserve the timestamp, we could later calculate flow rates and things like that
  core::ThreadSafeDeque<std::uint64_t> pulse_queue_;

  // Thread-safe hand-off between the two threads. Each entry is one pulse's
  // timestamp in nanoseconds. We keep the timestamp (not just a count) so flow
  // rate could be calculated later from the time between pulses.
  std::atomic<std::uint64_t> pulse_count_{0};   // Running total of pulses. The consumer increments it; main reads it.

  // Threads are declared LAST. C++ destroys members in reverse order, so the
  // threads are destroyed before the queue and everything above them.
  std::thread reader_thread_;
  std::thread processor_thread_;
};

}  // namespace comms
