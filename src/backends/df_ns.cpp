// SPDX-License-Identifier: Apache-2.0
#include "df_ns.h"

#include <deep_filter/deep_filter.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace klear {

struct DeepFilterNs::Impl {
    DFState* st = nullptr;
    std::size_t frame_len = 0;       // DF-native frame length (samples @ 48k)
    std::vector<float> in_f;         // input buffer (float32 ±1)
    std::vector<float> out_f;        // output buffer
    // Running stats
    double snr_sum = 0.0;
    std::uint64_t frame_count = 0;
    double reduction_accum_db = 0.0;
};

DeepFilterNs::DeepFilterNs() : impl_(std::make_unique<Impl>()) {}
DeepFilterNs::DeepFilterNs(const Options& opts)
    : impl_(std::make_unique<Impl>()), opts_(opts) {}

DeepFilterNs::~DeepFilterNs() {
    if (impl_ && impl_->st) df_free(impl_->st);
}

bool DeepFilterNs::init(int sample_rate, std::string* error) {
    if (sample_rate != 48000) {
        if (error) *error =
            "deepfilternet backend currently requires sample_rate=48000";
        return false;
    }
    sample_rate_ = sample_rate;

    // df_create in libdeepfilter 0.5.7 dereferences the path without a null
    // check (capi.rs:90), so we must always pass a real filesystem path.
    // Default to the model installed alongside the library.
    static constexpr const char* kDefaultModel =
        "/usr/local/share/deepfilternet/models/DeepFilterNet3_onnx.tar.gz";
    const char* path = opts_.model_path.empty() ? kDefaultModel
                                                : opts_.model_path.c_str();
    const char* lvl = opts_.log_level.empty() ? "error"
                                              : opts_.log_level.c_str();
    impl_->st = df_create(path, opts_.atten_lim_db, lvl);
    if (!impl_->st) {
        if (error) *error = "df_create failed (model missing or incompatible)";
        return false;
    }
    df_set_post_filter_beta(impl_->st, opts_.post_filter_beta);

    impl_->frame_len = df_get_frame_length(impl_->st);
    if (impl_->frame_len == 0) {
        if (error) *error = "df_get_frame_length returned 0";
        return false;
    }
    impl_->in_f.resize(impl_->frame_len);
    impl_->out_f.resize(impl_->frame_len);
    return true;
}

void DeepFilterNs::process(int16_t* in_out, std::size_t num_samples) {
    if (!impl_ || !impl_->st) return;
    const std::size_t fl = impl_->frame_len;
    if (num_samples == 0 || num_samples % fl != 0) return;

    const float inv = 1.0f / 32768.0f;
    const std::size_t frames = num_samples / fl;
    for (std::size_t f = 0; f < frames; ++f) {
        int16_t* seg = in_out + f * fl;

        // int16 -> float
        double rms_in = 0.0;
        for (std::size_t i = 0; i < fl; ++i) {
            float v = seg[i] * inv;
            impl_->in_f[i] = v;
            rms_in += static_cast<double>(v) * v;
        }

        float snr = df_process_frame(impl_->st, impl_->in_f.data(),
                                     impl_->out_f.data());

        // float -> int16 with clipping + running stats
        double rms_out = 0.0;
        for (std::size_t i = 0; i < fl; ++i) {
            float y = impl_->out_f[i];
            if (y >  1.0f) y =  1.0f;
            if (y < -1.0f) y = -1.0f;
            seg[i] = static_cast<int16_t>(y * 32767.0f);
            rms_out += static_cast<double>(y) * y;
        }

        // Track average reduction: 10*log10(rms_in / rms_out)
        if (rms_out > 1e-12 && rms_in > 1e-12) {
            impl_->reduction_accum_db += 10.0 * std::log10(rms_in / rms_out);
        }
        impl_->snr_sum += snr;
        impl_->frame_count++;
    }
}

int DeepFilterNs::algorithmic_delay_ms() const {
    // DFN3 has ~20 ms of lookahead in the DeepFilter stage. This is our
    // provisional value — we will measure it empirically via an impulse
    // response test and refine it once the harness reports the measured
    // group delay. Tracked as a TODO in the baseline benchmark.
    return 20;
}

void DeepFilterNs::get_stats(NsStats* out) const {
    if (!impl_ || impl_->frame_count == 0) {
        *out = NsStats{};
        return;
    }
    out->reduction_db =
        impl_->reduction_accum_db / static_cast<double>(impl_->frame_count);
    out->speech_prob = 0.0;  // not computed yet
}

}  // namespace klear
