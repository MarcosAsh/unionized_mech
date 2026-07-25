#define _POSIX_C_SOURCE 200809L

#include "core/timer.h"

#include <time.h>

namespace core {

u64 Timer::now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
}

}  // namespace core
