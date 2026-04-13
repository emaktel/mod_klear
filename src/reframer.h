// SPDX-License-Identifier: Apache-2.0
//
// Reframer: splits arbitrary-size input blocks into fixed-size output blocks.
//
// FreeSWITCH media bugs deliver audio in ptime-sized chunks (commonly 20 ms).
// Our backends require exactly 10 ms blocks. In the common case (ptime is a
// multiple of 10 ms) the reframer produces whole blocks without buffering; we
// still implement a proper ring buffer so we are robust to odd ptimes, late
// packets, or codec-specific frame sizes in the future.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace klear {

class Reframer {
public:
    // block_size = samples per output frame (e.g. sample_rate / 100 for 10 ms).
    // capacity_samples should be ≥ block_size; the ring grows if a single push
    // exceeds it.
    explicit Reframer(std::size_t block_size, std::size_t capacity_samples = 0);

    // Append samples to the internal buffer.
    void push(const int16_t* samples, std::size_t n);

    // True while a complete block can be popped.
    bool has_block() const { return size_ >= block_size_; }

    // Copy the oldest block_size_ samples into out (which must have room for
    // block_size_ samples) and advance the read pointer. Returns false if no
    // full block is available.
    bool pop(int16_t* out);

    std::size_t block_size() const { return block_size_; }
    std::size_t pending() const { return size_; }
    void reset();

private:
    void ensure_capacity(std::size_t required);

    std::vector<int16_t> buf_;
    std::size_t block_size_;
    std::size_t head_ = 0;   // read index
    std::size_t tail_ = 0;   // write index
    std::size_t size_ = 0;   // valid samples
};

}  // namespace klear
