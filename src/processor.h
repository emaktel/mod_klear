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

    // Feed the far-end reference (what FreeSWITCH is about to send towards
    // the remote leg) to the echo canceller. This must be called once per
    // render frame so AEC can learn the echo path. num_samples must be a
    // multiple of block_size(). Non-modifying.
    void process_render(const int16_t* write, std::size_t num_samples);

    // Clean a near-end capture frame in place (what FreeSWITCH just
    // received from the remote leg). Runs AEC, then NS, both respecting
    // the hot-toggle flags. num_samples must be a multiple of block_size().
    void process_capture(int16_t* read, std::size_t num_samples);

    // Convenience wrapper: calls process_render then process_capture. Used
    // by the offline harness where both streams are available at once. The
    // FreeSWITCH media bug path uses the split calls above.
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
    bool ensure_alignment(std::size_t num_samples);

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
