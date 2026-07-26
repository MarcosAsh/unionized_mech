#pragma once

#include "core/types.h"

namespace core {

/// A read cursor over a text buffer for token-based parsers.
struct Cursor {
    const char* p = nullptr;
    const char* end = nullptr;
};

/// Skip whitespace, newlines, and '#' comments to end of line.
inline void skip_space(Cursor& c) {
    while (c.p < c.end) {
        const char ch = *c.p;
        if (ch == '#') {
            while (c.p < c.end && *c.p != '\n') {
                ++c.p;
            }
        } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++c.p;
        } else {
            break;
        }
    }
}

/// Read the next word into `buf`. Returns false at end of input.
[[nodiscard]] inline bool next_token(Cursor& c, char* buf, u64 cap) {
    skip_space(c);
    if (c.p >= c.end) {
        return false;
    }
    u64 n = 0;
    while (c.p < c.end && n + 1 < cap) {
        const char ch = *c.p;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '#') {
            break;
        }
        buf[n++] = ch;
        ++c.p;
    }
    buf[n] = '\0';
    return n > 0;
}

/// Parse a decimal float like -12.5. Hand-rolled so parsing is deterministic
/// and locale-free, unlike sscanf. Returns false when no number is present.
[[nodiscard]] inline bool parse_f32(Cursor& c, f32* out) {
    skip_space(c);
    const char* p = c.p;
    f32 sign = 1.0f;
    if (p < c.end && (*p == '-' || *p == '+')) {
        sign = *p == '-' ? -1.0f : 1.0f;
        ++p;
    }
    bool any = false;
    f32 value = 0.0f;
    while (p < c.end && *p >= '0' && *p <= '9') {
        value = value * 10.0f + static_cast<f32>(*p - '0');
        ++p;
        any = true;
    }
    if (p < c.end && *p == '.') {
        ++p;
        f32 scale = 0.1f;
        while (p < c.end && *p >= '0' && *p <= '9') {
            value += static_cast<f32>(*p - '0') * scale;
            scale *= 0.1f;
            ++p;
            any = true;
        }
    }
    if (!any) {
        return false;
    }
    c.p = p;
    *out = sign * value;
    return true;
}

}  // namespace core
