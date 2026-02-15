#pragma once
// ============================================================================
// Lock-free SPMC Ring Buffer
//
// Single Producer (capture thread) writes PCM frames.
// Multiple Consumers (render threads) read independently, each maintaining
// their own read cursor.  The producer never blocks.
//
// Layout: power-of-two sized byte buffer, 64-byte aligned for cache-line
// and SIMD friendliness.  Wrap-around uses bitwise masking.
//
// Sequencing: A monotonic 64-bit write counter (in bytes) is published
// atomically with release semantics.  Consumers load with acquire.
// Each consumer tracks its own read position; if it falls behind by more
// than the buffer size, frames are dropped (overrun) -- acceptable for
// real-time audio where old data is useless.
// ============================================================================

#include "Common.h"
#include <cstring>
#include <algorithm>
#include <new>          // std::hardware_destructive_interference_size

namespace msbt {

class RingBuffer {
public:
    static constexpr uint32_t kCapacityBytes = kRingBytes;
    static constexpr uint32_t kCapacityMask  = kCapacityBytes - 1;
    static_assert((kCapacityBytes & kCapacityMask) == 0,
                  "Ring capacity must be a power of two.");

    // -- Producer interface ---------------------------------------------------

    // Write `byteCount` bytes into the ring.  Returns the byte offset
    // (pre-wrap) where the write started, usable as AudioPacket::ringOffset.
    uint32_t Write(const void* data, uint32_t byteCount) noexcept {
        const uint64_t wpos = writePos_.load(std::memory_order_relaxed);
        const uint32_t offset = static_cast<uint32_t>(wpos & kCapacityMask);
        const auto* src = static_cast<const uint8_t*>(data);

        // Handle wrap-around with two memcpy calls
        const uint32_t firstChunk = std::min(byteCount, kCapacityBytes - offset);
        std::memcpy(buffer_ + offset, src, firstChunk);
        if (firstChunk < byteCount) {
            std::memcpy(buffer_, src + firstChunk, byteCount - firstChunk);
        }

        // Publish new write position (release so consumers see the data)
        writePos_.store(wpos + byteCount, std::memory_order_release);
        return offset;
    }

    // Current write head (monotonic byte count).
    uint64_t WriteHead() const noexcept {
        return writePos_.load(std::memory_order_acquire);
    }

    // -- Consumer interface ---------------------------------------------------

    // Each consumer owns a ConsumerCursor.  This is a plain uint64_t tracking
    // the consumer's monotonic read position.  It lives on the consumer's
    // cache line (not shared with the producer).
    struct alignas(64) ConsumerCursor {
        uint64_t readPos = 0;
    };

    // Read up to `maxBytes` into `dest`.  Returns actual bytes read.
    // Advances the cursor.  If the consumer fell behind, skips to the
    // earliest available data (overrun recovery).
    uint32_t Read(ConsumerCursor& cursor, void* dest, uint32_t maxBytes) const noexcept {
        const uint64_t wpos = writePos_.load(std::memory_order_acquire);
        uint64_t rpos = cursor.readPos;

        // Overrun detection: if producer has lapped us, snap forward
        if (wpos - rpos > kCapacityBytes) {
            rpos = wpos - kCapacityBytes;
        }

        const uint64_t available = wpos - rpos;
        const uint32_t toRead = static_cast<uint32_t>(
            std::min(static_cast<uint64_t>(maxBytes), available));

        if (toRead == 0) return 0;

        const uint32_t offset = static_cast<uint32_t>(rpos & kCapacityMask);
        auto* dst = static_cast<uint8_t*>(dest);

        const uint32_t firstChunk = std::min(toRead, kCapacityBytes - offset);
        std::memcpy(dst, buffer_ + offset, firstChunk);
        if (firstChunk < toRead) {
            std::memcpy(dst + firstChunk, buffer_, toRead - firstChunk);
        }

        cursor.readPos = rpos + toRead;
        return toRead;
    }

    // Peek: read without advancing the cursor.
    uint32_t Peek(const ConsumerCursor& cursor, void* dest, uint32_t maxBytes) const noexcept {
        ConsumerCursor tmp = cursor;
        return Read(tmp, dest, maxBytes);
    }

    // How many bytes are available for this consumer to read.
    uint64_t Available(const ConsumerCursor& cursor) const noexcept {
        const uint64_t wpos = writePos_.load(std::memory_order_acquire);
        uint64_t rpos = cursor.readPos;
        if (wpos - rpos > kCapacityBytes) {
            rpos = wpos - kCapacityBytes;
        }
        return wpos - rpos;
    }

    // Reset a consumer to the current write head (discard all pending data).
    void ResetConsumer(ConsumerCursor& cursor) const noexcept {
        cursor.readPos = writePos_.load(std::memory_order_acquire);
    }

    // -- Direct buffer access (for zero-copy paths) --------------------------

    const uint8_t* RawBuffer() const noexcept { return buffer_; }
    uint8_t*       RawBuffer()       noexcept { return buffer_; }

    static uint32_t WrapOffset(uint64_t pos) noexcept {
        return static_cast<uint32_t>(pos & kCapacityMask);
    }

private:
    // The buffer itself -- 64-byte aligned for AVX-512 / cache-line ops
    alignas(64) uint8_t buffer_[kCapacityBytes]{};

    // Producer write position -- on its own cache line to avoid false sharing
    alignas(64) std::atomic<uint64_t> writePos_{0};
};

} // namespace msbt
