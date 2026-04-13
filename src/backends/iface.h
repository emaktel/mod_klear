// SPDX-License-Identifier: Apache-2.0
// mod_klear backend interfaces: pluggable AEC and NS backends.
//
// A "frame" throughout this module is a mono block of PCM16 samples whose
// duration equals the pipeline block size (10 ms). Sample counts are
// therefore always sample_rate / 100.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace klear {

struct AecStats {
    double erle_db = 0.0;           // echo return loss enhancement, averaged
    double echo_likelihood = 0.0;   // [0,1], APM echo likelihood
    int delay_estimate_ms = 0;      // AEC-estimated render->capture delay
};

struct NsStats {
    double reduction_db = 0.0;      // average estimated noise reduction
    double speech_prob = 0.0;       // [0,1]
};

// Base class for acoustic echo cancellers. Frames are 10 ms mono PCM16 at
// sample_rate. process_render() must be called with the corresponding far-end
// reference *before* process_capture() for the same time window.
class IAecBackend {
public:
    virtual ~IAecBackend() = default;

    virtual bool init(int sample_rate, std::string* error) = 0;
    virtual void process_render(const int16_t* render, std::size_t num_samples) = 0;
    virtual void process_capture(int16_t* in_out, std::size_t num_samples) = 0;

    virtual int algorithmic_delay_ms() const = 0;
    virtual void get_stats(AecStats* out) const = 0;

    virtual const char* name() const = 0;
};

// Base class for noise suppressors. Operates in place on 10 ms mono PCM16.
class INsBackend {
public:
    virtual ~INsBackend() = default;

    virtual bool init(int sample_rate, std::string* error) = 0;
    virtual void process(int16_t* in_out, std::size_t num_samples) = 0;

    virtual int algorithmic_delay_ms() const = 0;
    virtual void get_stats(NsStats* out) const = 0;

    virtual const char* name() const = 0;
};

}  // namespace klear
