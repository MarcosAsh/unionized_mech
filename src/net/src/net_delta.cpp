#include "net/net.h"

// Snapshot deltas. The world is 1576 bytes and a packet holds 1200, so a full
// snapshot cannot be sent at all; what makes it fit is that most of it does not
// change between ticks. Team, weapon, ammunition reserves and the whole of a
// standing player are identical tick to tick, and only what moved has to travel.
//
// The state is treated as an array of 32-bit words with a bitmask of which ones
// differ from the baseline. That is deliberately dumber than a per-field
// encoder: it needs no schema, so it cannot drift out of step with the struct
// it encodes, and adding a field to Character costs nothing here.

namespace net {

namespace {

/// Bytes of bitmask needed for `words` bits.
[[nodiscard]] constexpr u32 mask_bytes(u32 words) { return (words + 7u) / 8u; }

}  // namespace

u32 delta_encode(const void* baseline, const void* current, u32 size, u8* out,
                 u32 out_capacity) {
    if (size % 4 != 0 || size == 0) {
        return 0;  // callers pass whole structs; a tail byte would be a bug
    }
    const u32 words = size / 4;
    const u32 mask_size = mask_bytes(words);
    if (out_capacity < sizeof(u16) + mask_size) {
        return 0;
    }

    const u8* base = static_cast<const u8*>(baseline);
    const u8* cur = static_cast<const u8*>(current);

    // Word count first, so the far end can reject a snapshot encoded against a
    // struct of a different size rather than decoding it into nonsense.
    out[0] = static_cast<u8>(words & 0xFF);
    out[1] = static_cast<u8>((words >> 8) & 0xFF);
    u8* mask = out + sizeof(u16);
    for (u32 i = 0; i < mask_size; ++i) {
        mask[i] = 0;
    }

    u32 cursor = sizeof(u16) + mask_size;
    for (u32 w = 0; w < words; ++w) {
        const u32 at = w * 4;
        if (base[at] == cur[at] && base[at + 1] == cur[at + 1] &&
            base[at + 2] == cur[at + 2] && base[at + 3] == cur[at + 3]) {
            continue;
        }
        if (cursor + 4 > out_capacity) {
            return 0;  // too much changed to fit; the caller must fall back
        }
        mask[w / 8] = static_cast<u8>(mask[w / 8] | (1u << (w % 8)));
        out[cursor++] = cur[at];
        out[cursor++] = cur[at + 1];
        out[cursor++] = cur[at + 2];
        out[cursor++] = cur[at + 3];
    }
    return cursor;
}

bool delta_decode(const void* baseline, const u8* in, u32 in_size, void* out, u32 size) {
    if (size % 4 != 0 || size == 0 || in_size < sizeof(u16)) {
        return false;
    }
    const u32 words = size / 4;
    const u32 stated = static_cast<u32>(in[0]) | (static_cast<u32>(in[1]) << 8);
    if (stated != words) {
        return false;  // encoded against a different struct than we expect
    }
    const u32 mask_size = mask_bytes(words);
    if (in_size < sizeof(u16) + mask_size) {
        return false;
    }

    const u8* base = static_cast<const u8*>(baseline);
    u8* dst = static_cast<u8*>(out);
    const u8* mask = in + sizeof(u16);
    u32 cursor = sizeof(u16) + mask_size;

    for (u32 w = 0; w < words; ++w) {
        const u32 at = w * 4;
        if ((mask[w / 8] & (1u << (w % 8))) == 0) {
            dst[at] = base[at];
            dst[at + 1] = base[at + 1];
            dst[at + 2] = base[at + 2];
            dst[at + 3] = base[at + 3];
            continue;
        }
        // Every changed word must actually be present; a truncated packet has
        // to be rejected rather than half-applied over the baseline.
        if (cursor + 4 > in_size) {
            return false;
        }
        dst[at] = in[cursor++];
        dst[at + 1] = in[cursor++];
        dst[at + 2] = in[cursor++];
        dst[at + 3] = in[cursor++];
    }
    return cursor == in_size;  // trailing bytes mean we disagree about the format
}

}  // namespace net
