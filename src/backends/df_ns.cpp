// SPDX-License-Identifier: Apache-2.0
#include "df_ns.h"

#include <deep_filter/deep_filter.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace klear {

// Internal processing rate for DeepFilterNet.
static constexpr int kDfRateHz = 48000;

struct DeepFilterNs::Impl {
    DFState* st = nullptr;
    std::size_t frame_len = 0;       // DF-native frame length (samples @ 48k)
    std::vector<float> df_in;        // one DF frame, float32 ±1
    std::vector<float> df_out;       // one DF frame, float32 ±1
    // Running stats
    double snr_sum = 0.0;
    std::uint64_t frame_count = 0;
    double reduction_accum_db = 0.0;

    // Resampling state — only populated when channel_rate != 48 kHz.
    soxr_t up = nullptr;       // channel_rate -> 48k
    soxr_t down = nullptr;     // 48k -> channel_rate
    std::vector<float> up_ring;     // upsampler output awaiting DF
    std::size_t up_count = 0;
    std::vector<float> down_ring;   // DF output awaiting downsample
    std::size_t down_count = 0;
    std::vector<float> out_ring;    // downsampled output awaiting caller
    std::size_t out_count = 0;

    // Scratch for int16<->float conversion (sized to input block at
    // channel rate).
    std::vector<float> block_f_in;
    std::vector<float> block_f_out;
};

DeepFilterNs::DeepFilterNs() : impl_(std::make_unique<Impl>()) {}
DeepFilterNs::DeepFilterNs(const Options& opts)
    : impl_(std::make_unique<Impl>()), opts_(opts) {}

DeepFilterNs::~DeepFilterNs() {
    if (!impl_) return;
    if (impl_->st) df_free(impl_->st);
    if (impl_->up) soxr_delete(impl_->up);
    if (impl_->down) soxr_delete(impl_->down);
}

bool DeepFilterNs::init(int sample_rate, std::string* error) {
    if (sample_rate != 8000 && sample_rate != 16000 &&
        sample_rate != 32000 && sample_rate != 48000) {
        if (error) *error =
            "deepfilternet backend supports 8000/16000/32000/48000 Hz";
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
    impl_->df_in.resize(impl_->frame_len);
    impl_->df_out.resize(impl_->frame_len);

    const std::size_t channel_block = static_cast<std::size_t>(sample_rate) / 100;
    impl_->block_f_in.resize(channel_block);
    impl_->block_f_out.resize(channel_block);

    if (sample_rate != kDfRateHz) {
        // Generous ring capacity: several DF frames so we never overflow on
        // transient size mismatches, plus filter tails.
        const std::size_t up_cap = impl_->frame_len * 8;
        const std::size_t down_cap = impl_->frame_len * 8;
        const std::size_t out_cap = channel_block * 32;
        impl_->up_ring.resize(up_cap);
        impl_->down_ring.resize(down_cap);
        impl_->out_ring.resize(out_cap);

        soxr_error_t serr = nullptr;
        // Quality target: SOXR_QQ (quick cubic). Longer-filter qualities
        // (LQ/MQ/HQ) add 80-300 ms of group delay at integer ratios like
        // 6:1, which busts the pipeline budget. At phone bandwidth (0-4 k
        // at 8 k input) cubic interpolation preserves the signal perfectly
        // — the full soxr quality sweep in test/reports/soxr_quality_sweep
        // showed SOXR_QQ holding PESQ-nb 4.55 and STOI 1.00 at 8 kHz with
        // only 10 ms of added delay, identical quality to HQ without the
        // 140 ms tax.
        soxr_quality_spec_t qspec = soxr_quality_spec(SOXR_QQ, 0);
        soxr_io_spec_t iospec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        impl_->up = soxr_create(sample_rate, kDfRateHz, 1, &serr,
                                &iospec, &qspec, nullptr);
        if (!impl_->up) {
            if (error) *error = std::string("soxr up init: ") +
                                (serr ? serr : "unknown");
            return false;
        }
        impl_->down = soxr_create(kDfRateHz, sample_rate, 1, &serr,
                                  &iospec, &qspec, nullptr);
        if (!impl_->down) {
            if (error) *error = std::string("soxr down init: ") +
                                (serr ? serr : "unknown");
            return false;
        }

        // Skip pre-warming: the process() path has pass-through fallback
        // for the first ~20 ms until the resampler output ring fills,
        // which is imperceptible and keeps init simple.
    }
    return true;
}

// Native-rate path: channel rate already equals 48 kHz, feed DF directly.
static void run_df_native(DeepFilterNs::Impl& impl,
                          int16_t* in_out, std::size_t num_samples) {
    const std::size_t fl = impl.frame_len;
    const float inv = 1.0f / 32768.0f;
    const std::size_t frames = num_samples / fl;
    for (std::size_t f = 0; f < frames; ++f) {
        int16_t* seg = in_out + f * fl;
        double rms_in = 0.0;
        for (std::size_t i = 0; i < fl; ++i) {
            float v = seg[i] * inv;
            impl.df_in[i] = v;
            rms_in += static_cast<double>(v) * v;
        }
        float snr = df_process_frame(impl.st, impl.df_in.data(),
                                     impl.df_out.data());
        double rms_out = 0.0;
        for (std::size_t i = 0; i < fl; ++i) {
            float y = impl.df_out[i];
            if (y >  1.0f) y =  1.0f;
            if (y < -1.0f) y = -1.0f;
            seg[i] = static_cast<int16_t>(y * 32767.0f);
            rms_out += static_cast<double>(y) * y;
        }
        if (rms_out > 1e-12 && rms_in > 1e-12) {
            impl.reduction_accum_db += 10.0 * std::log10(rms_in / rms_out);
        }
        impl.snr_sum += snr;
        impl.frame_count++;
    }
}

// Resampled path: upsample channel-rate input to 48 kHz, process through
// DF, downsample back. Maintains three ring buffers (upsampler output,
// DF output, downsampler output) to decouple the variable-output-count
// behaviour of soxr from the fixed-size DF frames and the fixed-size
// caller blocks.
static void run_df_resampled(DeepFilterNs::Impl& impl,
                             int16_t* in_out, std::size_t num_samples) {
    const std::size_t fl = impl.frame_len;
    const float inv = 1.0f / 32768.0f;

    // 1) int16 -> float (channel rate)
    for (std::size_t i = 0; i < num_samples; ++i) {
        impl.block_f_in[i] = in_out[i] * inv;
    }

    // 2) upsample to 48 kHz into up_ring
    std::size_t up_in_done = 0, up_out_done = 0;
    soxr_process(impl.up, impl.block_f_in.data(), num_samples, &up_in_done,
                 impl.up_ring.data() + impl.up_count,
                 impl.up_ring.size() - impl.up_count, &up_out_done);
    impl.up_count += up_out_done;

    // 3) Run DF on every complete 48k frame, accumulating output into
    //    down_ring.
    while (impl.up_count >= fl) {
        double rms_in = 0.0;
        for (std::size_t i = 0; i < fl; ++i) {
            float v = impl.up_ring[i];
            impl.df_in[i] = v;
            rms_in += static_cast<double>(v) * v;
        }
        float snr = df_process_frame(impl.st, impl.df_in.data(),
                                     impl.df_out.data());
        double rms_out = 0.0;
        if (impl.down_count + fl > impl.down_ring.size()) {
            // Overflow protection: drop the oldest frame to make room.
            std::memmove(impl.down_ring.data(),
                         impl.down_ring.data() + fl,
                         (impl.down_count - fl) * sizeof(float));
            impl.down_count -= fl;
        }
        for (std::size_t i = 0; i < fl; ++i) {
            float y = impl.df_out[i];
            if (y >  1.0f) y =  1.0f;
            if (y < -1.0f) y = -1.0f;
            impl.down_ring[impl.down_count + i] = y;
            rms_out += static_cast<double>(y) * y;
        }
        impl.down_count += fl;
        if (rms_out > 1e-12 && rms_in > 1e-12) {
            impl.reduction_accum_db += 10.0 * std::log10(rms_in / rms_out);
        }
        impl.snr_sum += snr;
        impl.frame_count++;

        // Drop the consumed input from the up_ring.
        std::memmove(impl.up_ring.data(),
                     impl.up_ring.data() + fl,
                     (impl.up_count - fl) * sizeof(float));
        impl.up_count -= fl;
    }

    // 4) Downsample what we have in down_ring into out_ring.
    std::size_t dn_in_done = 0, dn_out_done = 0;
    soxr_process(impl.down, impl.down_ring.data(), impl.down_count,
                 &dn_in_done,
                 impl.out_ring.data() + impl.out_count,
                 impl.out_ring.size() - impl.out_count, &dn_out_done);
    if (dn_in_done > 0) {
        std::memmove(impl.down_ring.data(),
                     impl.down_ring.data() + dn_in_done,
                     (impl.down_count - dn_in_done) * sizeof(float));
        impl.down_count -= dn_in_done;
    }
    impl.out_count += dn_out_done;

    // 5) Emit num_samples back to the caller. If we don't have enough
    //    downsampled output yet (startup transient), pad with the input
    //    block so we never produce a gap in audio — this is a no-op on the
    //    first block or two, then steady state kicks in.
    if (impl.out_count >= num_samples) {
        for (std::size_t i = 0; i < num_samples; ++i) {
            float y = impl.out_ring[i];
            if (y >  1.0f) y =  1.0f;
            if (y < -1.0f) y = -1.0f;
            in_out[i] = static_cast<int16_t>(y * 32767.0f);
        }
        std::memmove(impl.out_ring.data(),
                     impl.out_ring.data() + num_samples,
                     (impl.out_count - num_samples) * sizeof(float));
        impl.out_count -= num_samples;
    }
    // else: warmup — leave in_out unchanged (pass-through)
}

void DeepFilterNs::process(int16_t* in_out, std::size_t num_samples) {
    if (!impl_ || !impl_->st) return;
    if (num_samples == 0) return;
    const std::size_t channel_block =
        static_cast<std::size_t>(sample_rate_) / 100;
    if (num_samples % channel_block != 0) return;

    // Process one caller block at a time so the resampler ring logic stays
    // simple and the reduction stat is normalised per 10 ms frame.
    for (std::size_t off = 0; off < num_samples; off += channel_block) {
        int16_t* seg = in_out + off;
        if (sample_rate_ == kDfRateHz) {
            run_df_native(*impl_, seg, channel_block);
        } else {
            run_df_resampled(*impl_, seg, channel_block);
        }
    }
}

int DeepFilterNs::algorithmic_delay_ms() const {
    // DFN3's own frame lookahead + any resampler filter delay.
    // Measured values from the offline harness:
    //   48 kHz native: ~20 ms
    //   8/16/32 kHz with SOXR_QQ cubic resample: ~30 ms total
    if (sample_rate_ == kDfRateHz) return 20;
    return 30;
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
