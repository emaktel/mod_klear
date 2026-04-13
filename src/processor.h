// SPDX-License-Identifier: Apache-2.0
//
// Processor ties together the AEC and NS backends under a single
// per-channel processing loop. It owns no threads and is safe to drive from
// whichever thread FreeSWITCH hands the media bug to, plus concurrent
// hot-toggle calls from dialplan/API threads.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "backends/iface.h"

namespace klear {

struct ProcessorConfig {
    int sample_rate = 16000;  // 8000 / 16000 / 32000 / 48000
    bool aec_enabled = true;
    bool ns_enabled = true;
};

struct ProcessorStats {
    AecStats aec{};
    NsStats ns{};
    std::uint64_t frames_processed = 0;
    double cpu_seconds_total = 0.0;   // wall time spent in process()
};

class Processor {
public:
    Processor();
    ~Processor();

    // Takes ownership of the backends. On failure returns false and writes
    // an explanation to error.
    bool init(const ProcessorConfig& cfg,
              std::unique_ptr<IAecBackend> aec,
              std::unique_ptr<INsBackend> ns,
              std::string* error);

    // read:  samples incoming from remote (near-end / mic). Modified in place.
    // write: samples we are about to send to remote (far-end reference). Unmodified.
    // num_samples must be a multiple of block_size(); mismatched input is a
    // no-op (logged once). Typical inputs are 10/20/30/40 ms frames.
    void process(int16_t* read, const int16_t* write, std::size_t num_samples);

    // Thread-safe hot toggles.
    void set_aec_enabled(bool on) { aec_enabled_.store(on, std::memory_order_relaxed); }
    void set_ns_enabled(bool on)  { ns_enabled_.store(on, std::memory_order_relaxed); }
    bool aec_enabled() const { return aec_enabled_.load(std::memory_order_relaxed); }
    bool ns_enabled()  const { return ns_enabled_.load(std::memory_order_relaxed); }

    std::size_t block_size() const { return block_size_; }
    int sample_rate() const { return cfg_.sample_rate; }
    int algorithmic_delay_ms() const;

    void get_stats(ProcessorStats* out) const;

private:
    ProcessorConfig cfg_{};
    std::size_t block_size_ = 0;
    std::unique_ptr<IAecBackend> aec_;
    std::unique_ptr<INsBackend> ns_;
    std::atomic<bool> aec_enabled_{false};
    std::atomic<bool> ns_enabled_{false};
    mutable ProcessorStats stats_{};
    bool alignment_warned_ = false;
};

}  // namespace klear
