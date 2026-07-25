#include "core/assert.h"

#include "core/log.h"

#include <cstdlib>

namespace core {

void assert_fail(const char* file, i32 line, const char* expr, const char* msg) {
    if (msg != nullptr) {
        log_errorf("assertion failed: %s (%s:%d): %s", expr, file, line, msg);
    } else {
        log_errorf("assertion failed: %s (%s:%d)", expr, file, line);
    }
    std::abort();
}

}  // namespace core
