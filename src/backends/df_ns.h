// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "iface.h"

#include <memory>

namespace klear {

// DeepFilterNet (DFN3) neural noise suppression via libdeepfilter.
//
// DeepFilterNet's native rate is 48 kHz. For channels running at other
// rates (8k narrowband, 16k wideband, 32k superwideband) this backend
// transparently resamples with libsoxr: int16 channel-rate in → float
// upsample to 48k → DF → downsample to channel rate → int16 out. Input
// frames must still be an integer number of block_size samples
// (sample_rate / 100).
class DeepFilterNs : public INsBackend {
public:
    struct Options {
        // Path to a DFN model tar.gz. When empty the bundled DFN3 model
        // compiled into libdeepfilter is used.
        std::string model_path;
        // Max attenuation in dB applied by DF. atten=30 with post-filter
        // off is the empirically-tuned default from test/reports/df_sweep:
        // 38.8 dB far-end ERLE combined with AEC3 while holding near-end
        // PESQ preservation at 4.14 (vs 4.55 AEC-only and 4.64 passthrough).
        float atten_lim_db = 30.0f;
        // Post-filter beta in [0.0, ~0.05]. 0 disables the post filter.
        // Disabled by default — measurement showed it hurts preservation
        // without meaningfully improving echo cancellation.
        float post_filter_beta = 0.0f;
        // DF logging verbosity: "none", "error", "warn", "info", "debug".
        std::string log_level = "error";
    };

    DeepFilterNs();
    explicit DeepFilterNs(const Options& opts);
    ~DeepFilterNs() override;

    bool init(int sample_rate, std::string* error) override;
    void process(int16_t* in_out, std::size_t num_samples) override;

    // DFN3 algorithmic delay plus any libsoxr filter delay imposed by the
    // input resampling stage. At 48 kHz native the value is ~20 ms. At
    // 8/16/32 kHz the SOXR_QQ resampler adds roughly 10 ms end-to-end, so
    // the total pipeline delay (AEC + NS + resampler) stays at or below
    // 40 ms for all supported rates.
    int algorithmic_delay_ms() const override;
    void get_stats(NsStats* out) const override;
    const char* name() const override { return "deepfilternet"; }

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    Options opts_;
    int sample_rate_ = 0;
};

}  // namespace klear
