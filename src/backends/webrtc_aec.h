// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "iface.h"

#include <memory>

namespace klear {

// WebRTC APM-based acoustic echo canceller using AEC3 + high-pass filter.
// All other APM features (NS, AGC, VAD) are disabled so noise suppression
// can be handled by a separate NS backend (e.g. DeepFilterNet).
class WebrtcAec : public IAecBackend {
public:
    struct Options {
        bool high_pass_filter = true;
        // Render->capture delay hint handed to APM on init. 0 is a safe
        // default; APM's internal delay estimator handles the actual offset.
        int initial_stream_delay_ms = 0;
    };

    WebrtcAec();
    explicit WebrtcAec(const Options& opts);
    ~WebrtcAec() override;

    bool init(int sample_rate, std::string* error) override;
    void process_render(const int16_t* render, std::size_t num_samples) override;
    void process_capture(int16_t* in_out, std::size_t num_samples) override;

    int algorithmic_delay_ms() const override { return 10; }  // AEC3 ~10 ms
    void get_stats(AecStats* out) const override;
    const char* name() const override { return "webrtc-aec3"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Options opts_;
    int sample_rate_ = 0;
};

}  // namespace klear
