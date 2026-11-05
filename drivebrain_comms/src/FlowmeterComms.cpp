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

    request.offsets[0] = gpio_line_offset;
    request.num_lines = 1;

    request.config.flags =
        GPIO_V2_LINE_FLAG_INPUT |
        GPIO_V2_LINE_FLAG_EDGE_RISING;

    std::snprintf(
        request.consumer,
        sizeof(request.consumer),
        "flowmeter"
    );

    // Request the GPIO line from the Linux kernel.
    if (ioctl(chip_fd_, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        int error = errno;
        close(chip_fd_);
        chip_fd_ = -1;

        throw std::runtime_error(
            std::string("Failed to request GPIO line: ") +
            std::strerror(error));
    }

    line_fd_ = request.fd;
    
    // Allows both threads to run
    running_ = true;

    try {
        // Starts the processor first so it is ready for incoming pulses.
        processor_thread_ = std::thread(&FlowmeterComms::process_data_worker, this);

        // Starts reading GPIO events.
        reader_thread_ = std::thread(&FlowmeterComms::read_pin_worker, this);
    }

    catch (...) {
       {
            std::lock_guard<std::mutex> lock(pulse_queue_.mtx);
            running_ = false;
        }
        pulse_queue_.cv.notify_all();

        if (processor_thread_.joinable()) {
            processor_thread_.join();
        }
        close(line_fd_);
        close(chip_fd_);
        line_fd_ = -1;
        chip_fd_ = -1;
        throw;
}

// Destructor: Stops both threads before releasing GPIO resources.
FlowmeterComms::~FlowmeterComms() {
    // Set the flag while holding the queue's mutex so the consumer can't
    // miss the wakeup between checking its condition and going to sleep.
    {
        std::lock_guard<std::mutex> lock(pulse_queue_.mtx);
        running_ = false;
    }
    pulse_queue_.cv.notify_all();

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (processor_thread_.joinable()) {
        processor_thread_.join();
    }

    // Closes GPIO file descriptors
    if (line_fd_ >= 0) {
        close(line_fd_);
        line_fd_ = -1;
    }
    if (chip_fd_ >= 0) {
        close(chip_fd_);
        chip_fd_ = -1;
    }
}

// Reader thread: waits for rising-edge GPIO events
// and pushes their timestamps into the shared queue.
void FlowmeterComms::read_pin_worker() {

    while (true) {
        std::uint64_t timestamp = 0;

        {
            std::unique_lock<std::mutex> lock(pulse_queue_.mtx);

            // Wait until a pulse arrives or shutdown is requested.
            pulse_queue_.cv.wait(lock, [this] {
                return !pulse_queue_.deque.empty() || !running_;
            });

            // Finish only after all queued pulses are processed.
            if (pulse_queue_.deque.empty() && !running_) {
                break;
            }

            // Retrieve the oldest timestamp.
            timestamp = pulse_queue_.deque.front();
            pulse_queue_.deque.pop_front();
        }

        // Increment the total pulse count.
        std::uint64_t count = pulse_count_.fetch_add(1) + 1;



}

// CONSUMER: sleeps until pulses arrive, takes them out under the lock,
// then handles them after releasing it.
void FlowmeterComms::process_data_worker() {
    while (true) {
        std::deque<std::uint64_t> batch;

        {
            std::unique_lock<std::mutex> lock(pulse_queue_.mtx);
            pulse_queue_.cv.wait(lock, [this] {
                return !pulse_queue_.deque.empty() || !running_;
            });

            if (pulse_queue_.deque.empty() && !running_) {
                break;
            }

            // Move everything out while holding the lock.
            batch.swap(pulse_queue_.deque);
        }  // lock released here

        // Handle the pulses with the lock released.
        pulse_count_.fetch_add(batch.size());
    }
}

    
    
    

std::uint64_t FlowmeterComms::get_pulse_count() const {
    return pulse_count_.load();
}

} //namespace comms
