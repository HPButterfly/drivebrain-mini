// Purpose: Implements the constructor, destructor, get_pulse_count(), 
// and the two worker functions from FlowmeterComms.hpp
#include <FlowmeterComms.hpp>
#include <linux/gpio.h> //  Configures the line and reading edge events
#include <sys/ioctl.h> //  Sends configuration requests to the GPIO chip
#include <fcntl.h> //  Opens /dev/gpiochipN
#include <unistd.h> //  Closes file descriptors and reading events
#include <poll.h> //  Waits for a pulse with a timeout

#include <cerrno> // Reports why a system call failed (errno, strerror)
#include <cstring> //  Reports why a system call failed (errno, strerror)
#include <stdexcept> //  Throws an exception if setup fails (for example, a missing chip
#include <utility> //  std::move, for taking items out of the queue
#include <vector> //  Holds the batch of pulses you move out of the deque

#include <cstdint>  // Provides fixed-size integer types, such as std::uint64_t, for storing pulse counts and timestamps.
#include <ctime>    // Provides time-related functions and types.
#include <mutex>    // Provides mutexes and locks to safely share data between threads.
#include <chrono>   // Provides time-related utilities for measuring durations and working with timestamps.
#include <memory>   // Provides smart pointers, such as std::make_shared, for managing dynamically allocated objects.
#include <cstdio>   // Provides C-style input/output functions, such as std::snprintf, for formatting text.

namespace comms {

// Constructor: opens GPIO, configures rising-edge detection,
// and starts the reader and processor threads.
FlowmeterComms::FlowmeterComms(
    const std::string& gpio_chip_path,
    std::uint32_t gpio_line_offset) {

    // Open the GPIO chip.
    chip_fd_ = open(gpio_chip_path.c_str(), O_RDONLY | O_CLOEXEC);

    if (chip_fd_ < 0) {
        throw std::runtime_error(
            std::string("Failed to open GPIO chip: ") +
            std::strerror(errno));
    }

    // Configure a request for one GPIO line.
    gpio_v2_line_request request{};

std::uint64_t FlowmeterComms::get_pulse_count() const {
    return pulse_count_.load();


}

} //namespace comms
