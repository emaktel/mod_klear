// SPDX-License-Identifier: Apache-2.0
#include "webrtc_aec.h"

#include "api/audio/audio_processing.h"
#include "api/audio/audio_processing_statistics.h"
#include "api/scoped_refptr.h"

namespace klear {

struct WebrtcAec::Impl {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig stream_cfg{0, 1};
    webrtc::AudioProcessingStats last_stats;
};

WebrtcAec::WebrtcAec() : impl_(std::make_unique<Impl>()) {}
WebrtcAec::WebrtcAec(const Options& opts)
    : impl_(std::make_unique<Impl>()), opts_(opts) {}
WebrtcAec::~WebrtcAec() = default;

bool WebrtcAec::init(int sample_rate, std::string* error) {
    // APM only supports the specific sample rates it resamples to internally.
    // Any rate in [8000, 48000] is accepted by ProcessStream (it resamples),
    // but we expose the typical phone rates.
    if (sample_rate != 8000 && sample_rate != 16000 &&
        sample_rate != 32000 && sample_rate != 48000) {
        if (error) *error = "webrtc-aec: unsupported sample rate";
        return false;
    }
    sample_rate_ = sample_rate;

    webrtc::AudioProcessing::Config cfg;
    cfg.echo_canceller.enabled = true;
    cfg.echo_canceller.mobile_mode = false;        // AEC3, not AECm
    cfg.high_pass_filter.enabled = opts_.high_pass_filter;
    cfg.noise_suppression.enabled = false;         // handled by NS backend
    cfg.gain_controller1.enabled = false;
    cfg.gain_controller2.enabled = false;

    impl_->apm = webrtc::AudioProcessingBuilder().SetConfig(cfg).Create();
    if (!impl_->apm) {
        if (error) *error = "webrtc-aec: APM create failed";
        return false;
    }

    impl_->stream_cfg = webrtc::StreamConfig(sample_rate, 1);
    impl_->apm->set_stream_delay_ms(opts_.initial_stream_delay_ms);
    return true;
}

void WebrtcAec::process_render(const int16_t* render, std::size_t /*num_samples*/) {
    impl_->apm->ProcessReverseStream(render, impl_->stream_cfg,
                                     impl_->stream_cfg,
                                     const_cast<int16_t*>(render));
}

void WebrtcAec::process_capture(int16_t* in_out, std::size_t /*num_samples*/) {
    impl_->apm->ProcessStream(in_out, impl_->stream_cfg,
                              impl_->stream_cfg, in_out);
}

void WebrtcAec::get_stats(AecStats* out) const {
    if (!impl_ || !impl_->apm) {
        *out = AecStats{};
        return;
    }
    impl_->last_stats = impl_->apm->GetStatistics();
    out->erle_db = impl_->last_stats.echo_return_loss_enhancement.value_or(0.0);
    out->echo_likelihood =
        impl_->last_stats.residual_echo_likelihood.value_or(0.0);
    out->delay_estimate_ms = impl_->last_stats.delay_ms.value_or(0);
}

}  // namespace klear
