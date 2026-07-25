#include "core/log.h"

#include <cstdarg>
#include <cstdio>

namespace core {

static void vlog(std::FILE* out, const char* fmt, va_list args) {
    std::vfprintf(out, fmt, args);
    std::fputc('\n', out);
    std::fflush(out);
}

void log_message(LogLevel level, const char* msg) {
    std::FILE* out = (level == LogLevel::Error) ? stderr : stdout;
    std::fputs(msg, out);
    std::fputc('\n', out);
    std::fflush(out);
}

void log_info(const char* msg) { log_message(LogLevel::Info, msg); }

void log_error(const char* msg) { log_message(LogLevel::Error, msg); }

void log_infof(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stdout, fmt, args);
    va_end(args);
}

void log_errorf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(stderr, fmt, args);
    va_end(args);
}

}  // namespace core
