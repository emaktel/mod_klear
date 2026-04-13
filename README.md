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

## Results and presets

mod_klear ships three measurement-tuned presets. The right one depends on
whether you care more about raw echo removal or near-end speech
preservation during double-talk. All numbers below are averages on the
Microsoft AEC Challenge ICASSP 2022 test set, scored by the offline
harness (`test/runner/bench.py`, report `test/reports/presets.md`).
AECMOS is Microsoft's reference doubletalk MOS model, 1-5 higher-is-
better.

| preset        | use case | far ERLE | near PESQ | DT echo MOS | DT deg MOS | RTF  | delay |
|---------------|---|--:|--:|--:|--:|--:|--:|
| `agent` *(default)* | AI agents, ASR, speech preservation | **17.4 dB** | **4.13** | **3.47** | **3.26** | 0.31 | 30 ms |
| `telephony`   | Classic human-human, echo must die | **38.8 dB** | 4.14 | 4.34 | 2.01 | 0.33 | 30 ms |
| `aec_only`    | Minimum CPU, single-talk dominant | 16.8 dB | 4.55 | 4.14 | 1.87 | 0.02 | 10 ms |
| pass-through (baseline) | — | 0 dB | 4.64 | 2.81 | 3.96 | 0 | 0 ms |

Reading the table: the `agent` preset uses DeepFilterNet as a neural
echo+noise suppressor (no AEC3). It gives up some peak echo cancellation
(17 dB vs 39 dB) but during doubletalk it preserves near-end speech at
**3.26 deg MOS versus 2.01** for the classic AEC3-aggressive path, a full
1.25 MOS improvement. For an LLM or ASR listening on the other side of
the call, that preservation is the difference between a reliable
transcript and a degraded one.

All three presets run at every common phone rate — 8 k, 16 k, 32 k, and
48 k — via a transparent libsoxr (SOXR_QQ cubic) resampling stage inside
the DF backend. Delay is 30 ms at 48 kHz native (AEC3 10 ms + DFN3 20 ms);
~40 ms at other rates (+ 10 ms for the resampler), still well inside the
50 ms conversational comfort budget. ITU G.114 flags > 150 ms one-way as
the start of perceptible latency.

Full rate-sweep: `cat test/reports/rate_sweep_qq.md` after running
`python3 test/runner/bench.py --rates 8000,16000,32000,48000`.

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

| variable          | type   | default          | effect                                                     |
|-------------------|--------|------------------|------------------------------------------------------------|
| `klear_preset`    | enum   | `agent` (global) | `agent` / `telephony` / `aec_only` — see presets table     |
| `klear_aec`       | bool   | preset-driven    | force AEC3 on/off (overrides preset)                       |
| `klear_ns`        | bool   | preset-driven    | force DF3 NS on/off (overrides preset)                     |
| `klear_hpf`       | bool   | `true`           | enable AEC3 high-pass filter (only when AEC is on)         |
| `klear_ns_atten`  | float  | `30.0` dB        | DF attenuation limit; higher = more aggressive             |

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

## FusionPBX demo dialplans

If you run FusionPBX the repo ships with a ready-to-install dialplan bundle
in `examples/fusionpbx_dialplans.sql` that exposes the module as five star
codes in the `global` context (so every domain inherits them):

| code   | name              | what it does |
|--------|-------------------|---|
| `*9200` | klear-control     | baseline — plain `echo`, no klear, so you can hear the raw input |
| `*9201` | klear-agent       | `klear_preset=agent` (DF-only) + echo |
| `*9202` | klear-telephony   | `klear_preset=telephony` (AEC3 + DF aggressive) + echo |
| `*9203` | klear-aec-only    | `klear_preset=aec_only` + echo |
| `*9204` | klear-hot-toggle  | runs `examples/klear_hot_toggle.lua`, which flips `ns`/`aec` every 5 s so you can hear the pipeline change mid-call |

Install:

```sh
# (1) copy the Lua helper somewhere FreeSWITCH can find it
sudo cp examples/klear_hot_toggle.lua /usr/share/freeswitch/scripts/

# (2) load the dialplan rows (global context, idempotent)
PGPASSWORD=... psql -h DB_HOST -U fusionpbx -d fusionpbx \
    -f examples/fusionpbx_dialplans.sql

# (3) flush FusionPBX's dialplan cache and reload FS XML
sudo rm -f /var/cache/fusionpbx/dialplan*
sudo fs_cli -x "reloadxml"
```

Then dial `*9200`..`*9204` from any domain extension. Watch the live log
for `klear: attached` / `klear: ns=...` / `klear: aec=...` lines:

```sh
fs_cli -x "console loglevel info"
tail -f /var/log/freeswitch/freeswitch.log | grep klear
```

To auto-load the module on every FS restart, add `<load module="mod_klear"/>`
somewhere inside the `<modules>` block of `modules.conf.xml`.

## Live channel smoke test

After building and loading the module you can exercise the full live path
— origination, media bug attach, frame processing, detach, stats event —
with one command:

```sh
make live-test
```

This runs `test/live_smoke.sh`, which originates a `null/nothing &echo`
channel via `fs_cli`, attaches mod_klear, lets a couple of seconds of
audio flow through, and verifies from the FS log that the callback
processed at least 10 capture frames. Expected output:

```
[live_smoke] PASS: 101 capture frames processed through mod_klear on live FS channel
```

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
