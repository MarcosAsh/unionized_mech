#include "net/net.h"

// Sequence numbers and acknowledgements. No sockets here on purpose: this is
// the part with the awkward wrapping arithmetic, and keeping it pure means it
// can be tested by feeding it headers rather than by dropping real packets.

namespace net {

bool sequence_newer(u16 a, u16 b) {
    // Half the range is the tie-breaker: `a` is newer either when it is simply
    // larger, or when it is much smaller, which only happens across a wrap.
    constexpr u16 HALF = 32768;
    return (a > b && a - b <= HALF) || (b > a && b - a > HALF);
}

namespace {

/// Fold `seq` into a window whose newest entry is `newest`, held in `bits`
/// where bit i means (newest - 1 - i) was seen. Returns false when `seq` is
/// already recorded or has fallen out of the window entirely.
bool record(u16 seq, u16* newest, u32* bits) {
    if (seq == *newest) {
        return false;  // duplicate of the newest
    }
    if (sequence_newer(seq, *newest)) {
        const u32 shift = static_cast<u16>(seq - *newest);
        if (shift >= Connection::ACK_HISTORY) {
            *bits = 0;  // the whole window is older than the gap
        } else {
            // The old newest becomes the first bit of history behind the new one.
            *bits = (*bits << shift) | (1u << (shift - 1));
        }
        *newest = seq;
        return true;
    }
    const u32 behind = static_cast<u16>(*newest - seq);
    if (behind > Connection::ACK_HISTORY) {
        return false;  // too old to represent
    }
    const u32 bit = 1u << (behind - 1);
    if ((*bits & bit) != 0) {
        return false;  // already recorded
    }
    *bits |= bit;
    return true;
}

/// True when `seq` sits in the window described by `newest` and `bits`.
[[nodiscard]] bool present(u16 seq, u16 newest, u32 bits) {
    if (seq == newest) {
        return true;
    }
    if (sequence_newer(seq, newest)) {
        return false;  // newer than anything recorded
    }
    const u32 behind = static_cast<u16>(newest - seq);
    if (behind > Connection::ACK_HISTORY) {
        return false;
    }
    return (bits & (1u << (behind - 1))) != 0;
}

}  // namespace

Header Connection::next_header() {
    Header h{};
    h.protocol = PROTOCOL_ID;
    h.sequence = local_sequence_++;
    // Tell the far end what we have heard from it. Before anything has arrived
    // there is nothing to acknowledge, and a zero here would falsely claim
    // sequence 0.
    h.ack = have_remote_ ? remote_sequence_ : static_cast<u16>(0xFFFFu);
    h.ack_bits = have_remote_ ? received_bits_ : 0u;
    return h;
}

bool Connection::on_received(const Header& header) {
    if (header.protocol != PROTOCOL_ID) {
        return false;
    }
    // Learn what the far end has heard from us, even if this packet is a
    // duplicate: its acknowledgements may still be news.
    if (header.ack != 0xFFFFu || header.ack_bits != 0) {
        if (!have_acked_) {
            acked_sequence_ = header.ack;
            acked_bits_ = header.ack_bits;
            have_acked_ = true;
        } else if (sequence_newer(header.ack, acked_sequence_)) {
            const u32 shift = static_cast<u16>(header.ack - acked_sequence_);
            acked_bits_ = shift >= ACK_HISTORY
                              ? header.ack_bits
                              : ((acked_bits_ << shift) | (1u << (shift - 1))) | header.ack_bits;
            acked_sequence_ = header.ack;
        } else {
            acked_bits_ |= header.ack_bits;
        }
    }

    if (!have_remote_) {
        remote_sequence_ = header.sequence;
        received_bits_ = 0;
        have_remote_ = true;
        return true;
    }
    return record(header.sequence, &remote_sequence_, &received_bits_);
}

bool Connection::acked(u16 sequence) const {
    return have_acked_ && present(sequence, acked_sequence_, acked_bits_);
}

}  // namespace net
