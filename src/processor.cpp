// SPDX-License-Identifier: Apache-2.0
#include "processor.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace klear {

Processor::Processor() = default;
Processor::~Processor() = default;

bool Processor::init(const ProcessorConfig& cfg,
                     std::unique_ptr<IAecBackend> aec,
                     std::unique_ptr<INsBackend> ns,
                     std::string* error) {
    cfg_ = cfg;
    block_size_ = static_cast<std::size_t>(cfg_.sample_rate) / 100;
    if (block_size_ == 0) {
        if (error) *error = "invalid sample rate";
        return false;
    }
    aec_ = std::move(aec);
    ns_ = std::move(ns);
    if (aec_ && !aec_->init(cfg_.sample_rate, error)) return false;
    if (ns_  && !ns_->init(cfg_.sample_rate, error))  return false;
    aec_enabled_.store(cfg_.aec_enabled, std::memory_order_relaxed);
    ns_enabled_.store(cfg_.ns_enabled,   std::memory_order_relaxed);
    return true;
}

void Processor::process(int16_t* read, const int16_t* write, std::size_t num_samples) {
    if (num_samples == 0) return;
    if (num_samples % block_size_ != 0) {
        if (!alignment_warned_) {
            std::fprintf(stderr,
                "klear: input size %zu not a multiple of block %zu (sr=%d); "
                "skipping processing\n",
                num_samples, block_size_, cfg_.sample_rate);
            alignment_warned_ = true;
        }
        return;
    }

    auto t0 = std::chrono::steady_clock::now();

    const bool run_aec = aec_ && aec_enabled_.load(std::memory_order_relaxed);
    const bool run_ns  = ns_  && ns_enabled_.load(std::memory_order_relaxed);

    const std::size_t blocks = num_samples / block_size_;
    for (std::size_t i = 0; i < blocks; ++i) {
        int16_t* r = read + i * block_size_;
        const int16_t* w = write + i * block_size_;

        if (run_aec) {
            // Render (far-end reference) must be supplied before capture for
            // the AEC to align the echo path. APM's internal adaptive delay
            // estimator handles small residual offsets.
            aec_->process_render(w, block_size_);
            aec_->process_capture(r, block_size_);
        }
        if (run_ns) {
            ns_->process(r, block_size_);
        }
        stats_.frames_processed++;
    }

    auto t1 = std::chrono::steady_clock::now();
    stats_.cpu_seconds_total +=
        std::chrono::duration<double>(t1 - t0).count();
}

int Processor::algorithmic_delay_ms() const {
    int d = 0;
    if (aec_ && aec_enabled()) d += aec_->algorithmic_delay_ms();
    if (ns_  && ns_enabled())  d += ns_->algorithmic_delay_ms();
    return d;
}

void Processor::get_stats(ProcessorStats* out) const {
    *out = stats_;
    if (aec_) aec_->get_stats(&out->aec);
    if (ns_)  ns_->get_stats(&out->ns);
}

}  // namespace klear
