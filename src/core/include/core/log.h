#pragma once

#include "core/types.h"

namespace core {

/// Severity of a log line.
enum class LogLevel : u8 { Info, Error };

/// Write one already-formatted line at `level`. Info goes to stdout, Error to stderr.
void log_message(LogLevel level, const char* msg);

/// Log a plain info string.
void log_info(const char* msg);

/// Log a plain error string.
void log_error(const char* msg);

/// printf-style info log. Format string is checked at compile time.
void log_infof(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/// printf-style error log. Format string is checked at compile time.
void log_errorf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace core
