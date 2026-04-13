// SPDX-License-Identifier: Apache-2.0
#include "reframer.h"

#include <algorithm>
#include <cstring>

namespace klear {

Reframer::Reframer(std::size_t block_size, std::size_t capacity_samples)
    : block_size_(block_size) {
    std::size_t cap = std::max(capacity_samples, block_size * 4);
    buf_.resize(cap);
}

void Reframer::ensure_capacity(std::size_t required) {
    if (required <= buf_.size()) return;
    std::vector<int16_t> grown(required * 2);
    if (size_ > 0) {
        // linearise existing data into grown starting at index 0
        if (head_ + size_ <= buf_.size()) {
            std::memcpy(grown.data(), buf_.data() + head_, size_ * sizeof(int16_t));
        } else {
            std::size_t first = buf_.size() - head_;
            std::memcpy(grown.data(), buf_.data() + head_, first * sizeof(int16_t));
            std::memcpy(grown.data() + first, buf_.data(), (size_ - first) * sizeof(int16_t));
        }
    }
    buf_ = std::move(grown);
    head_ = 0;
    tail_ = size_;
}

void Reframer::push(const int16_t* samples, std::size_t n) {
    ensure_capacity(size_ + n);
    std::size_t cap = buf_.size();
    std::size_t first = std::min(n, cap - tail_);
    std::memcpy(buf_.data() + tail_, samples, first * sizeof(int16_t));
    std::size_t rest = n - first;
    if (rest > 0) {
        std::memcpy(buf_.data(), samples + first, rest * sizeof(int16_t));
    }
    tail_ = (tail_ + n) % cap;
    size_ += n;
}

bool Reframer::pop(int16_t* out) {
    if (size_ < block_size_) return false;
    std::size_t cap = buf_.size();
    std::size_t first = std::min(block_size_, cap - head_);
    std::memcpy(out, buf_.data() + head_, first * sizeof(int16_t));
    std::size_t rest = block_size_ - first;
    if (rest > 0) {
        std::memcpy(out + first, buf_.data(), rest * sizeof(int16_t));
    }
    head_ = (head_ + block_size_) % cap;
    size_ -= block_size_;
    return true;
}

void Reframer::reset() {
    head_ = tail_ = size_ = 0;
}

}  // namespace klear
