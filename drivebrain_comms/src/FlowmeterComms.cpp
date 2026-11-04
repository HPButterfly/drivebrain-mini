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
#include <cstdint>
#include <ctime>
#include <mutex>
#include <chrono>

namespace comms {

std::uint64_t FlowmeterComms::get_pulse_count() const {
    return pulse_count_.load();


}

} //namespace comms
