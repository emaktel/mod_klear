// SPDX-License-Identifier: Apache-2.0
//
// klear_test: offline runner for the mod_klear pipeline.
//
// Usage:
//   klear_test --near NEAR.wav --far FAR.wav --out OUT.wav --stats OUT.json
//              [--rate 16000] [--aec on|off] [--ns on|off] [--hpf on|off]
//              [--ns-level 0..4] [--backend-ns deepfilter|null]
//
// NEAR is the signal that would reach FreeSWITCH as the "capture" direction
// from the remote leg (contains echo + noise + desired speech). FAR is the
// far-end reference that would be sent out towards the remote leg. Both must
// be mono, PCM16, at the same sample rate, which must match --rate.
//
// The pipeline invoked here is identical to the one run inside mod_klear, so
// any metric produced against klear_test is a valid predictor of what the
// module will do on a live call.
#include "processor.h"
#include "backends/df_ns.h"
#include "backends/null_aec.h"
#include "backends/null_ns.h"
#include "backends/webrtc_aec.h"

#include <sndfile.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string near_path;
    std::string far_path;
    std::string out_path;
    std::string stats_path;
    int sample_rate = 16000;
    bool aec = true;
    bool ns = false;
    bool hpf = true;
    std::string backend_ns = "null";
};

[[noreturn]] void die(const char* msg) {
    std::fprintf(stderr, "klear_test: %s\n", msg);
    std::exit(2);
}

bool parse_bool(const char* s) {
    return std::strcmp(s, "on") == 0 || std::strcmp(s, "1") == 0 ||
           std::strcmp(s, "true") == 0;
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) die("missing value for flag");
            (void)flag;
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--near")) a.near_path = next("near");
        else if (!std::strcmp(argv[i], "--far")) a.far_path = next("far");
        else if (!std::strcmp(argv[i], "--out")) a.out_path = next("out");
        else if (!std::strcmp(argv[i], "--stats")) a.stats_path = next("stats");
        else if (!std::strcmp(argv[i], "--rate")) a.sample_rate = std::atoi(next("rate"));
        else if (!std::strcmp(argv[i], "--aec")) a.aec = parse_bool(next("aec"));
        else if (!std::strcmp(argv[i], "--ns")) a.ns = parse_bool(next("ns"));
        else if (!std::strcmp(argv[i], "--hpf")) a.hpf = parse_bool(next("hpf"));
        else if (!std::strcmp(argv[i], "--backend-ns")) a.backend_ns = next("backend-ns");
        else {
            std::fprintf(stderr, "klear_test: unknown arg %s\n", argv[i]);
            die("bad args");
        }
    }
    if (a.near_path.empty() || a.far_path.empty()) die("--near and --far required");
    return a;
}

// Read a mono wav into a vector. Resamples not supported — file SR must match.
std::vector<int16_t> load_mono_wav(const std::string& path, int expected_rate) {
    SF_INFO info{};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    if (!sf) {
        std::fprintf(stderr, "sf_open %s: %s\n", path.c_str(), sf_strerror(nullptr));
        std::exit(2);
    }
    if (info.samplerate != expected_rate) {
        std::fprintf(stderr,
            "rate mismatch in %s: file=%d expected=%d\n",
            path.c_str(), info.samplerate, expected_rate);
        sf_close(sf);
        std::exit(2);
    }
    std::vector<int16_t> interleaved(info.frames * info.channels);
    sf_readf_short(sf, interleaved.data(), info.frames);
    sf_close(sf);
    if (info.channels == 1) return interleaved;
    // Downmix to mono.
    std::vector<int16_t> mono(info.frames);
    for (sf_count_t i = 0; i < info.frames; ++i) {
        int32_t acc = 0;
        for (int c = 0; c < info.channels; ++c)
            acc += interleaved[i * info.channels + c];
        mono[i] = static_cast<int16_t>(acc / info.channels);
    }
    return mono;
}

void save_mono_wav(const std::string& path, const std::vector<int16_t>& data,
                   int sample_rate) {
    SF_INFO info{};
    info.samplerate = sample_rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!sf) {
        std::fprintf(stderr, "sf_open(write) %s: %s\n", path.c_str(),
                     sf_strerror(nullptr));
        std::exit(2);
    }
    sf_writef_short(sf, data.data(), data.size());
    sf_close(sf);
}

std::unique_ptr<klear::INsBackend> make_ns(const std::string& which) {
    if (which == "null") return std::make_unique<klear::NullNs>();
    if (which == "deepfilter") return std::make_unique<klear::DeepFilterNs>();
    std::fprintf(stderr, "klear_test: unsupported --backend-ns=%s\n", which.c_str());
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    auto near_buf = load_mono_wav(a.near_path, a.sample_rate);
    auto far_buf = load_mono_wav(a.far_path, a.sample_rate);

    const std::size_t n = std::min(near_buf.size(), far_buf.size());
    near_buf.resize(n);
    far_buf.resize(n);

    klear::ProcessorConfig cfg;
    cfg.sample_rate = a.sample_rate;
    cfg.aec_enabled = a.aec;
    cfg.ns_enabled = a.ns;

    klear::WebrtcAec::Options aec_opts;
    aec_opts.high_pass_filter = a.hpf;

    klear::Processor proc;
    std::string err;
    if (!proc.init(cfg, std::make_unique<klear::WebrtcAec>(aec_opts),
                   make_ns(a.backend_ns), &err)) {
        std::fprintf(stderr, "init: %s\n", err.c_str());
        return 2;
    }

    const std::size_t block = proc.block_size();
    const std::size_t usable = (n / block) * block;

    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t off = 0; off < usable; off += block) {
        proc.process(near_buf.data() + off, far_buf.data() + off, block);
    }
    auto t1 = std::chrono::steady_clock::now();

    if (!a.out_path.empty()) {
        near_buf.resize(usable);
        save_mono_wav(a.out_path, near_buf, a.sample_rate);
    }

    klear::ProcessorStats s;
    proc.get_stats(&s);
    const double audio_seconds = static_cast<double>(usable) / a.sample_rate;
    const double wall_seconds =
        std::chrono::duration<double>(t1 - t0).count();
    const double rtf = audio_seconds > 0 ? wall_seconds / audio_seconds : 0.0;

    if (!a.stats_path.empty()) {
        FILE* fp = std::fopen(a.stats_path.c_str(), "w");
        if (!fp) {
            std::fprintf(stderr, "open stats: %s\n", a.stats_path.c_str());
            return 2;
        }
        std::fprintf(fp,
            "{\n"
            "  \"sample_rate\": %d,\n"
            "  \"samples_processed\": %zu,\n"
            "  \"audio_seconds\": %.6f,\n"
            "  \"wall_seconds\": %.6f,\n"
            "  \"rtf\": %.6f,\n"
            "  \"algorithmic_delay_ms\": %d,\n"
            "  \"aec\": {\n"
            "    \"enabled\": %s,\n"
            "    \"erle_db\": %.4f,\n"
            "    \"echo_likelihood\": %.4f,\n"
            "    \"delay_estimate_ms\": %d\n"
            "  },\n"
            "  \"ns\": {\n"
            "    \"enabled\": %s,\n"
            "    \"backend\": \"%s\",\n"
            "    \"reduction_db\": %.4f\n"
            "  }\n"
            "}\n",
            a.sample_rate, usable, audio_seconds, wall_seconds, rtf,
            proc.algorithmic_delay_ms(),
            a.aec ? "true" : "false",
            s.aec.erle_db, s.aec.echo_likelihood, s.aec.delay_estimate_ms,
            a.ns ? "true" : "false", a.backend_ns.c_str(),
            s.ns.reduction_db);
        std::fclose(fp);
    }

    std::printf("klear_test: audio=%.3fs wall=%.3fs rtf=%.4f erle=%.2fdB\n",
                audio_seconds, wall_seconds, rtf, s.aec.erle_db);
    return 0;
}
