# mod_klear

A FreeSWITCH module for low-latency voice cleanup on every channel.
mod_klear runs WebRTC AEC3 acoustic echo cancellation and DeepFilterNet 3
neural noise suppression as a single in-process pipeline. It is intended
for AI-agent deployments that need clean phone audio before ASR / LLM
inference, and for human-to-human calls that want to sound like a modern
conferencing app.

## Why this exists

FreeSWITCH has no maintained open-source module that provides
state-of-the-art AEC and neural noise suppression together. The bundled
`mod_spandsp` AEC is aimed at analog/FXO hardware, `mod_rnnoise` wraps the
classic (pre-neural) RNNoise, and there is no glue for AEC3. mod_klear
fills that gap with measurable, tunable, and hot-toggleable cleanup on
real phone audio.

## Results

Measured on the Microsoft AEC Challenge ICASSP 2022 test set using the
offline harness that shares its pipeline with the live module
(`test/runner/bench.py`, default config: DeepFilterNet3 atten_lim 30 dB,
post-filter off, AEC3 + high-pass on). The module works at every common
phone rate — narrowband (8 k), wideband (16 k), super-wideband (32 k), and
full-band (48 k) — via a transparent libsoxr resampling stage inside the
DF backend.

| rate   | ERLE dB  | PESQ       | STOI  | RTF   | delay |
|--------|---------:|-----------:|------:|------:|------:|
|  8 kHz | **38.0** | **4.28** nb | 0.977 | 0.245 | 40 ms |
| 16 kHz | 36.5     | 4.07 wb    | 0.984 | 0.281 | 40 ms |
| 32 kHz | 39.2     | 4.05 wb    | 0.983 | 0.267 | 40 ms |
| 48 kHz | 38.8     | 4.18 wb    | 0.986 | 0.239 | 30 ms |

ERLE is the whole-file power ratio on far-end single-talk clips (higher
is more echo removed). PESQ / STOI are computed against a silent-
reference near-end single-talk variant so they measure whether the
pipeline leaves clean speech alone when there is no echo or noise to
cancel. Per-channel wall-clock RTF is the fraction of real time spent in
the pipeline on a single CPU core.

For context: pass-through PESQ is 4.64, AEC-only (no NS) delivers 16.8 dB
ERLE. The full benchmark sweep is reproducible with `make test && cd
test/runner && python3 bench.py --rates 8000,16000,32000,48000`.

The pipeline delay is 30 ms at 48 kHz native (AEC3 10 ms + DFN3 20 ms) and
~40 ms at any other rate (+ SOXR_QQ 10 ms). Both fit inside the 50 ms
conversational-voice comfort budget, and ITU G.114 flags > 150 ms one-way
as the start of perceptible latency.

## Pipeline

```
remote → RTP → FS media bug ┬─ write stream ─► AEC3 render (reference)
                            └─ read stream  ─► AEC3 capture → DF3 NS → FS → codec → remote
```

- One `switch_media_bug` per channel with `SMBF_READ_REPLACE | SMBF_WRITE_REPLACE`.
- `WRITE_REPLACE` callback feeds the far-end reference (what FreeSWITCH is about to send to the remote leg) to AEC3.
- `READ_REPLACE` callback runs AEC3 capture then DF3 NS on the mic audio and writes the cleaned samples back in place.
- Both enables/disables are `std::atomic<bool>` checked per frame, so dialplan and ESL can flip AEC or NS mid-call without detaching the bug.

## Dependencies

- **FreeSWITCH** ≥ 1.10.x with `pkg-config freeswitch` working (`-dev` package or source build).
- **webrtc-audio-processing-2** (AEC3 — the package you want is the v2.x series, *not* the 0.3 that ships in Debian stable). Built from source; see below.
- **DeepFilterNet** `libdeepfilter` — built from source via `cargo cinstall`.
- **libsndfile** — only for the offline test harness.
- **Python 3.10+** with `numpy scipy soundfile pesq pystoi matplotlib tabulate` — only for the benchmark runner.

### Building webrtc-audio-processing-2 (AEC3)

```sh
sudo apt-get install -y meson ninja-build libabsl-dev
git clone --depth 1 https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing.git
cd webrtc-audio-processing
meson setup build --prefix=/usr/local --buildtype=release
ninja -C build
sudo ninja -C build install
sudo ldconfig
pkg-config --modversion webrtc-audio-processing-2   # expect 2.1 or newer
```

### Building DeepFilterNet (libdeepfilter + default model)

```sh
# Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
. "$HOME/.cargo/env"
cargo install cargo-c

# libdeepfilter
git clone --depth 1 https://github.com/Rikorose/DeepFilterNet.git
cd DeepFilterNet/libDF
cargo cinstall --release --prefix=/usr/local --features capi
sudo ldconfig

# Model file: df_create() in libdeepfilter 0.5.7 segfaults on a NULL path,
# so we install the DFN3 model to a known location and mod_klear defaults
# to it.
sudo mkdir -p /usr/local/share/deepfilternet/models
sudo cp ../models/DeepFilterNet3_onnx.tar.gz /usr/local/share/deepfilternet/models/
```

## Building mod_klear

```sh
git clone https://github.com/emaktech/mod_klear.git
cd mod_klear
make module               # produces build/mod_klear.so
make test                 # also builds the offline harness (needs libsndfile)
sudo make install         # installs the module and klear.conf.xml
fs_cli -x "load mod_klear"
```

## Usage

### Channel variables

Read at `klear start`; the boolean toggles are also re-read per frame after
being updated by the runtime `set` verb.

| variable          | type     | default       | effect                              |
|-------------------|----------|---------------|-------------------------------------|
| `klear_aec`       | bool     | `true`        | enable AEC3 echo cancellation       |
| `klear_ns`        | bool     | `true`        | enable DF3 noise suppression        |
| `klear_hpf`       | bool     | `true`        | enable AEC3 high-pass filter        |
| `klear_ns_atten`  | float dB | `30.0`        | DF attenuation limit                |

### Dialplan

```xml
<extension name="klear_to_agent">
  <condition field="destination_number" expression="^agent_(.*)$">
    <action application="answer"/>
    <action application="klear" data="start"/>
    <action application="bridge" data="user/$1"/>
  </condition>
</extension>
```

More examples are in `examples/dialplan.xml`.

### fs_cli / ESL

```sh
fs_cli -x "klear <uuid> start"
fs_cli -x "klear <uuid> set ns=off"
fs_cli -x "klear <uuid> set aec=off ns=off"   # temporarily disable both
fs_cli -x "klear <uuid> stop"
```

### Events

On channel destroy mod_klear fires a `CUSTOM klear::stats` event with:

```
klear-capture-frames   <int>      number of 10 ms capture blocks processed
klear-render-frames    <int>      number of 10 ms render blocks processed
klear-cpu-seconds      <float>    total wall time spent in the pipeline
klear-erle-db          <float>    ERLE from APM (see note below)
klear-ns-reduction-db  <float>    average NS power reduction
```

Note on the `klear-erle-db` value: webrtc-audio-processing-2's internal
ERLE statistic is unreliable across real signals — we have observed the
library report 0.18 dB while actually providing 40 + dB of cancellation
verified by offline RMS measurement. Trust the benchmark results, not the
per-call stat. We plan to compute ERLE ourselves when double-talk labels
are present.

## Tuning

The default `atten_lim_db = 30` is the empirically-chosen sweet spot from
`test/reports/df_sweep.md`: it still delivers >38 dB of echo removal while
holding near-end PESQ at 4.14. Raise for more aggressive suppression at
the cost of speech quality, lower for maximum near-end preservation. See
the sweep report for all four data points.

## Measurement / development workflow

```sh
# one-time dataset setup
cd test/fixtures
git clone https://github.com/microsoft/AEC-Challenge.git aec_challenge_src
cd aec_challenge_src && git lfs install --local
git lfs pull --include="datasets/test_set_icassp2022/*"
ln -sfn "$(pwd)/datasets" ../aec_challenge

# run the benchmark suite
cd ../../runner
python3 -m venv .venv && . .venv/bin/activate
pip install numpy scipy soundfile pesq pystoi matplotlib tabulate
python3 bench.py --run my-run
cat ../reports/my-run.md
```

`klear_test` is the C++ harness that drives the exact same pipeline as
the module from wav files:

```sh
./build/klear_test \
    --near path/to/mic.wav --far path/to/lpb.wav \
    --out /tmp/clean.wav --stats /tmp/stats.json \
    --rate 48000 --aec on --ns on --backend-ns deepfilter \
    --df-atten 30 --df-postfilter 0
```

## Limitations (v1)

- The `klear-erle-db` statistic is unreliable (see above).
- One channel at a time can have the bug attached (re-attaching is
  prevented, the way mod_stress does it).
- The pipeline is strictly mono. Stereo phone audio is out of scope.
- DeepFilterNet's default model ships inside libdeepfilter 0.5.7 but
  `df_create(NULL, ...)` segfaults on a null path rather than loading it,
  so the build installs `DeepFilterNet3_onnx.tar.gz` to
  `/usr/local/share/deepfilternet/models/` and the backend defaults to
  that path. If you want a custom model set `klear_ns_model` (future).

## License

Apache-2.0. See `LICENSE`.

Third-party components are separately licensed: webrtc-audio-processing
(BSD), DeepFilterNet (MIT/Apache-2.0), libsndfile (LGPL).

## Acknowledgements

- The Microsoft AEC Challenge corpus made the offline benchmark
  possible.
- The webrtc-audio-processing maintainers (PulseAudio project) keep the
  AEC3 stack alive as a standalone library.
- DeepFilterNet by Hendrik Schröter is the real star of the noise
  suppression stage.
