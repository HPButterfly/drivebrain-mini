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

// PRODUCER: waits for rising edges and pushes their timestamps to the queue.
void FlowmeterComms::read_pin_worker() {
    pollfd gpio_poll{};
    gpio_poll.fd = line_fd_;
    gpio_poll.events = POLLIN;
    while (running_) {

        // Wait for a GPIO event, checking periodically for shutdown.
        int result = poll(&gpio_poll, 1, 100);

        if(result < 0) {
            if (errno == EINTR) {
                continue;
            }

            spdlog::error("GPIO poll failed: {}", std::strerror(errno));
            running_ = false;
            break;

        }

        if (result == 0) {
            continue;
        }

        if (gpio_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            spdlog::error("GPIO poll reported an error");
            running_ = false;
            break;
        }

        if(!(gpio_poll.revents & POLLIN)) {
            continue;
        }

          // Read the GPIO event.
        gpio_v2_line_event event{};

        ssize_t bytes_read = read(
            line_fd_,
            &event,
            sizeof(event)
        );

        if (bytes_read < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }

            spdlog::error("GPIO read failed: {}", std::strerror(errno));
            running_ = false;
            break;
        }

        if (bytes_read != static_cast<ssize_t>(sizeof(event)) {
            spdlog::error("Incomplete GPIO event received");
            running_ = false;
            break;
        }

        // Only process rising-edge events.
        if (event.id != GPIO_V2_LINE_EVENT_RISING_EDGE) {
            continue;
        }

        // The kernel provides the event timestamp in nanoseconds.
        std::uint64_t timestamp = event.timestamp_ns;

        // Add the timestamp to the shared queue.
        {
            std::lock_guard<std::mutex> lock(pulse_queue_.mtx);

            pulse_queue_.deque.push_back(timestamp);
        }

        // Notify the processor that a new pulse is available.
        pulse_queue_.cv.notify_one();
    }
}

// CONSUMER: sleeps until pulses arrive, takes them out under the lock,
// then handles them after releasing it.
void FlowmeterComms::process_data_worker() {
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

        // Create the Protobuf telemetry message.
        auto message =
            std::make_shared<drivebrain_msgs::FlowmeterData>();

        message->set_pulse_count(count);
        message->set_timestamp_ns(timestamp);

        // Send the message to Foxglove for live visualization.
        core::FoxgloveServer::instance().send_live_telem_msg(
            message,
            "/flowmeter"
        );

        // Record the same message in the MCAP file.
        core::MCAPLogger::instance().log_msg(
            message,
            "/flowmeter"
        );
    }
}
    

std::uint64_t FlowmeterComms::get_pulse_count() const {
    return pulse_count_.load();
}

} //namespace comms
