// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "iface.h"

#include <memory>

namespace klear {

// DeepFilterNet (DFN3) neural noise suppression via libdeepfilter.
//
// v1 constraint: only sample_rate = 48000 is supported directly. For narrow-
// and wide-band phone channels we will add a libsoxr resampling stage in a
// follow-up; benchmarks run at 48k on the MS AEC Challenge corpus for now.
class DeepFilterNs : public INsBackend {
public:
    struct Options {
        // Path to a DFN model tar.gz. When empty the bundled DFN3 model
        // compiled into libdeepfilter is used.
        std::string model_path;
        // Maximum attenuation in dB applied by DF. Typical 30-100. Higher =
        // more aggressive suppression but more risk of speech damage.
        float atten_lim_db = 100.0f;
        // Post-filter beta in [0.0, ~0.05]. 0 disables the post filter.
        float post_filter_beta = 0.02f;
        // DF logging verbosity: "none", "error", "warn", "info", "debug".
        std::string log_level = "error";
    };

    DeepFilterNs();
    explicit DeepFilterNs(const Options& opts);
    ~DeepFilterNs() override;

    bool init(int sample_rate, std::string* error) override;
    void process(int16_t* in_out, std::size_t num_samples) override;

    int algorithmic_delay_ms() const override;
    void get_stats(NsStats* out) const override;
    const char* name() const override { return "deepfilternet"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Options opts_;
    int sample_rate_ = 0;
};

}  // namespace klear
