#pragma once
#include <linux/gpio.h>

#include <DriverBus.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace comms {

class FlowmeterComms {
 public:
  // Constructor opens/configures the GPIO line and starts both threads.
  FlowmeterComms(const std::string& gpio_chip_path,
                 std::uint32_t gpio_line_offset);

  // Destructor stops and joins the threads, then closes the file descriptors.
  ~FlowmeterComms();

  // Owns threads and file descriptors, so copying is not allowed.
  FlowmeterComms(const FlowmeterComms&) = delete;
  FlowmeterComms& operator=(const FlowmeterComms&) = delete;

  // Called by main to read the latest value.
  std::uint64_t get_pulse_count() const;

 private:
  void read_pin_worker();      // producer: waits for pulses, pushes to queue
  void process_data_worker();  // consumer: waits on queue, handles pulses

  int chip_fd_ = -1;
  int line_fd_ = -1;

  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> pulse_count_{0};

  // Instantiate the threadSafeDeque; we only count timestamps for this project, but
  // If we preserve the timestamp, we could later calculate flow rates and things like that
  core::ThreadSafeDeque<std::uint64_t> pulse_queue_;

  // Threads go LAST so they are destroyed before everything above.
  // Instantiate pulse_count_ that the consumer increments and main reads 
  std::atomic<std::uint64_t> pulse_count_{0};

  std::thread reader_thread_;
  std::thread processor_thread_;
};

}  // namespace comms
