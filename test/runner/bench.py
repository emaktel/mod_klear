#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
bench.py — benchmark runner for mod_klear.

Drives klear_test across the MS AEC Challenge ICASSP 2022 test set and
computes audio-quality metrics that klear_test (and APM itself) does not
provide reliably.

Metrics:
  * farend-singletalk ERLE: whole-file RMS ratio in dB. Because the near end
    is silent in these files, the entire mic signal is echo; the entire
    output should be quiet, so the ratio is a clean measure of how much
    echo energy we removed.
  * nearend-singletalk preservation: PESQ-WB, STOI, SI-SDR with mic as the
    clean reference and output as the degraded signal. We want these high —
    bad AEC / over-aggressive NS damages clean speech.
  * RTF and algorithmic delay: pulled straight from klear_test's JSON.

Usage:
  bench.py --run baseline
  bench.py --compare a b            # diff two previously-run reports
  bench.py --config configs.json    # run a specific set of configs
"""
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import time
from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Iterable

import numpy as np
import soundfile as sf
from pesq import pesq as _pesq
from pystoi import stoi as _stoi

REPO_ROOT = Path(__file__).resolve().parents[2]
KLEAR_TEST = REPO_ROOT / "build" / "klear_test"
FIXTURES = REPO_ROOT / "test" / "fixtures" / "aec_challenge" / "test_set_icassp2022"
REPORTS_DIR = REPO_ROOT / "test" / "reports"
REPORTS_DIR.mkdir(parents=True, exist_ok=True)


# ---------- helpers --------------------------------------------------------

def rms(x: np.ndarray) -> float:
    if x.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)) + 1e-20)


def rms_db(x: np.ndarray) -> float:
    return 20.0 * math.log10(rms(x) + 1e-20)


def si_sdr(reference: np.ndarray, estimate: np.ndarray) -> float:
    # Scale-invariant SDR (Le Roux et al. 2019). Assumes inputs are already
    # time-aligned — callers must compensate for any algorithmic delay first.
    reference = reference.astype(np.float64)
    estimate = estimate.astype(np.float64)
    ref_energy = np.dot(reference, reference) + 1e-20
    scale = np.dot(estimate, reference) / ref_energy
    proj = scale * reference
    noise = estimate - proj
    return 10.0 * math.log10(np.dot(proj, proj) / (np.dot(noise, noise) + 1e-20) + 1e-20)


def align_by_xcorr(reference: np.ndarray, estimate: np.ndarray,
                   max_shift: int) -> tuple[np.ndarray, np.ndarray, int]:
    """Find the integer sample shift in [-max_shift, +max_shift] that
    maximises the cross-correlation of reference and estimate, then return
    the trimmed pair. Positive shift means the estimate lags the reference
    (as happens with a causal AEC/NS: the estimate is the reference shifted
    forward in time by the algorithmic delay)."""
    if max_shift <= 0:
        n = min(len(reference), len(estimate))
        return reference[:n], estimate[:n], 0
    # Use a middle slice for the search to avoid fade-ins and to keep
    # xcorr cheap.
    n = min(len(reference), len(estimate))
    slice_len = min(n, 48000 * 3)   # 3 s at 48 k
    a0 = (n - slice_len) // 2
    a = reference[a0:a0 + slice_len].astype(np.float64)
    b = estimate [a0:a0 + slice_len].astype(np.float64)
    # np.correlate is O(n*max_shift); bound the shift to keep it cheap.
    best_shift, best_score = 0, -np.inf
    for s in range(-max_shift, max_shift + 1):
        if s >= 0:
            score = np.dot(a[:slice_len - s], b[s:slice_len])
        else:
            score = np.dot(a[-s:slice_len], b[:slice_len + s])
        if score > best_score:
            best_score = score
            best_shift = s
    # Apply the shift so est[shift:] lines up with ref[:-shift].
    if best_shift > 0:
        reference = reference[:-best_shift]
        estimate  = estimate[best_shift:]
    elif best_shift < 0:
        reference = reference[-best_shift:]
        estimate  = estimate[:best_shift]
    m = min(len(reference), len(estimate))
    return reference[:m], estimate[:m], best_shift


def load_pcm(path: Path) -> tuple[np.ndarray, int]:
    data, sr = sf.read(str(path), dtype="int16")
    if data.ndim > 1:
        data = data.mean(axis=1).astype(np.int16)
    return data, sr


# ---------- dataset discovery ---------------------------------------------

def discover_pairs(subdir: str, suffix: str) -> list[tuple[str, Path, Path]]:
    """Return [(id, mic_path, lpb_path)] for every doubletalk / singletalk pair
    actually present on disk (skipping un-smudged LFS pointer files)."""
    d = FIXTURES / subdir
    pairs: list[tuple[str, Path, Path]] = []
    if not d.is_dir():
        return pairs
    for mic in sorted(d.glob(f"*{suffix}_mic.wav")):
        # AEC Challenge convention: the loopback (far reference) shares the
        # same stem but ends in `_lpb.wav`. There is also a `..with-movement`
        # variant — we include both.
        stem = mic.name[: -len("_mic.wav")]
        lpb = d / f"{stem}_lpb.wav"
        if not lpb.is_file() or lpb.stat().st_size < 1024:
            continue
        if mic.stat().st_size < 1024:
            continue
        pairs.append((stem, mic, lpb))
    return pairs


def farend_singletalk_pairs() -> list[tuple[str, Path, Path]]:
    return discover_pairs("farend-singletalk", "farend-singletalk") + \
           discover_pairs("farend-singletalk", "farend-singletalk-with-movement")


def nearend_singletalk_pairs() -> list[tuple[str, Path, Path]]:
    return discover_pairs("nearend-singletalk", "nearend-singletalk") + \
           discover_pairs("nearend-singletalk", "nearend-singletalk-with-movement")


# ---------- running klear_test --------------------------------------------

@dataclass
class RunConfig:
    name: str
    aec: bool = True
    ns: bool = False
    hpf: bool = True
    backend_ns: str = "null"
    rate: int = 48000
    df_atten_db: float = 100.0
    df_post_filter_beta: float = 0.02


@dataclass
class FarendResult:
    file_id: str
    mic_rms_db: float
    out_rms_db: float
    erle_db: float
    rtf: float
    algo_delay_ms: int


@dataclass
class NearendResult:
    file_id: str
    pesq_wb: float
    stoi: float
    si_sdr_db: float
    rtf: float
    algo_delay_ms: int


@dataclass
class ConfigReport:
    config: RunConfig
    farend: list[FarendResult] = field(default_factory=list)
    nearend: list[NearendResult] = field(default_factory=list)

    def summary(self) -> dict:
        fe = self.farend
        ne = self.nearend
        def mean(key, lst):
            vs = [getattr(r, key) for r in lst]
            return float(np.mean(vs)) if vs else math.nan
        return {
            "config": asdict(self.config),
            "farend_singletalk": {
                "n": len(fe),
                "erle_db_mean": mean("erle_db", fe),
                "mic_rms_db_mean": mean("mic_rms_db", fe),
                "out_rms_db_mean": mean("out_rms_db", fe),
                "rtf_mean": mean("rtf", fe),
            },
            "nearend_singletalk": {
                "n": len(ne),
                "pesq_wb_mean": mean("pesq_wb", ne),
                "stoi_mean": mean("stoi", ne),
                "si_sdr_db_mean": mean("si_sdr_db", ne),
                "rtf_mean": mean("rtf", ne),
            },
            "algo_delay_ms": (fe[0].algo_delay_ms if fe else
                              (ne[0].algo_delay_ms if ne else -1)),
        }


def run_klear_test(cfg: RunConfig, near: Path, far: Path,
                   out_wav: Path, stats_json: Path) -> dict:
    cmd = [
        str(KLEAR_TEST),
        "--near", str(near), "--far", str(far),
        "--out", str(out_wav), "--stats", str(stats_json),
        "--rate", str(cfg.rate),
        "--aec", "on" if cfg.aec else "off",
        "--ns",  "on" if cfg.ns  else "off",
        "--hpf", "on" if cfg.hpf else "off",
        "--backend-ns", cfg.backend_ns,
        "--df-atten", str(cfg.df_atten_db),
        "--df-postfilter", str(cfg.df_post_filter_beta),
    ]
    t0 = time.time()
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"!! klear_test failed on {near.name}: {res.stderr}", file=sys.stderr)
        return {}
    stats = json.loads(stats_json.read_text())
    stats["_wall_invoke_s"] = time.time() - t0
    return stats


# ---------- per-scenario scoring ------------------------------------------

def score_farend(cfg: RunConfig, pairs: list[tuple[str, Path, Path]],
                 tmpdir: Path) -> list[FarendResult]:
    out: list[FarendResult] = []
    for i, (fid, mic, lpb) in enumerate(pairs):
        out_wav = tmpdir / f"{cfg.name}_fe_{i}.wav"
        stats_json = tmpdir / f"{cfg.name}_fe_{i}.json"
        stats = run_klear_test(cfg, mic, lpb, out_wav, stats_json)
        if not stats:
            continue
        mic_data, _ = load_pcm(mic)
        out_data, _ = load_pcm(out_wav)
        n = min(len(mic_data), len(out_data))
        mic_f = mic_data[:n].astype(np.float64) / 32768.0
        out_f = out_data[:n].astype(np.float64) / 32768.0
        mr = rms(mic_f); or_ = rms(out_f)
        erle = 20.0 * math.log10(max(mr, 1e-12) / max(or_, 1e-12))
        out.append(FarendResult(
            file_id=fid,
            mic_rms_db=20.0 * math.log10(mr + 1e-20),
            out_rms_db=20.0 * math.log10(or_ + 1e-20),
            erle_db=erle,
            rtf=float(stats.get("rtf", 0.0)),
            algo_delay_ms=int(stats.get("algorithmic_delay_ms", -1)),
        ))
    return out


def _silent_lpb_for(mic: Path, tmpdir: Path) -> Path:
    """Create a digitally-silent reference that matches the mic length / SR.
    Preservation tests need this because the real AEC-Challenge lpb files in
    the nearend-singletalk scenario are not silent — they contain prompt
    audio and ambient noise that legitimately reflects into the mic as echo.
    Using those as the `far` input means AEC correctly suppresses the
    echoey parts of the mic, and PESQ then scores the cleaner output as
    "degraded" against the echoey reference. That is a methodology bug, not
    an AEC bug. Feeding silence isolates the case we actually care about:
    `when there is no echo to cancel, does the pipeline leave the signal
    alone?`"""
    import soundfile as sf
    import numpy as np
    data, sr = sf.read(str(mic), dtype="int16")
    if data.ndim > 1:
        data = data[:, 0]
    silent = np.zeros_like(data)
    path = tmpdir / f"silent_{mic.stem}.wav"
    sf.write(str(path), silent, sr, subtype="PCM_16")
    return path


def score_nearend(cfg: RunConfig, pairs: list[tuple[str, Path, Path]],
                  tmpdir: Path) -> list[NearendResult]:
    out: list[NearendResult] = []
    for i, (fid, mic, _unused_lpb) in enumerate(pairs):
        lpb = _silent_lpb_for(mic, tmpdir)
        out_wav = tmpdir / f"{cfg.name}_ne_{i}.wav"
        stats_json = tmpdir / f"{cfg.name}_ne_{i}.json"
        stats = run_klear_test(cfg, mic, lpb, out_wav, stats_json)
        if not stats:
            continue
        mic_data, sr = load_pcm(mic)
        out_data, _  = load_pcm(out_wav)
        # Shift estimate forward by the pipeline's algorithmic delay so the
        # sample-aligned metrics (STOI, SI-SDR) measure quality rather than
        # alignment error. PESQ realigns internally so the shift is a no-op
        # for it.
        algo_ms = int(stats.get("algorithmic_delay_ms", 0))
        max_shift_samples = max(1, int((algo_ms + 10) * sr / 1000))
        mic_a, out_a, _ = align_by_xcorr(mic_data, out_data, max_shift_samples)
        mic_f = mic_a.astype(np.float64) / 32768.0
        out_f = out_a.astype(np.float64) / 32768.0
        # PESQ requires 8k or 16k; resample if needed.
        try:
            if sr in (8000, 16000):
                ref_p, deg_p, mode = mic_f, out_f, "wb" if sr == 16000 else "nb"
            else:
                from scipy.signal import resample_poly
                ref_p = resample_poly(mic_f, 16000, sr)
                deg_p = resample_poly(out_f, 16000, sr)
                mode = "wb"
            p = float(_pesq(16000 if mode == "wb" else 8000, ref_p, deg_p, mode))
        except Exception as e:
            p = math.nan
        try:
            s = float(_stoi(mic_f, out_f, sr, extended=False))
        except Exception:
            s = math.nan
        out.append(NearendResult(
            file_id=fid,
            pesq_wb=p,
            stoi=s,
            si_sdr_db=si_sdr(mic_f, out_f),
            rtf=float(stats.get("rtf", 0.0)),
            algo_delay_ms=int(stats.get("algorithmic_delay_ms", -1)),
        ))
    return out


# ---------- driver ---------------------------------------------------------

DEFAULT_CONFIGS = [
    RunConfig("passthrough", aec=False, ns=False, hpf=False),
    RunConfig("aec_only",    aec=True,  ns=False, hpf=True),
    RunConfig("aec_df_a100", aec=True,  ns=True,  hpf=True,
              backend_ns="deepfilter", df_atten_db=100, df_post_filter_beta=0.02),
    RunConfig("aec_df_a50",  aec=True,  ns=True,  hpf=True,
              backend_ns="deepfilter", df_atten_db=50,  df_post_filter_beta=0.02),
    RunConfig("aec_df_a30",  aec=True,  ns=True,  hpf=True,
              backend_ns="deepfilter", df_atten_db=30,  df_post_filter_beta=0.02),
    RunConfig("aec_df_a30_nopf", aec=True, ns=True, hpf=True,
              backend_ns="deepfilter", df_atten_db=30,  df_post_filter_beta=0.0),
]


def run(name: str, configs: list[RunConfig], max_per_scenario: int) -> Path:
    if not KLEAR_TEST.is_file():
        sys.exit(f"klear_test binary not found at {KLEAR_TEST}; build first")
    tmp = REPO_ROOT / "test" / "reports" / "_tmp"
    tmp.mkdir(parents=True, exist_ok=True)

    fe_pairs = farend_singletalk_pairs()[:max_per_scenario]
    ne_pairs = nearend_singletalk_pairs()[:max_per_scenario]
    print(f"farend-singletalk pairs : {len(fe_pairs)}")
    print(f"nearend-singletalk pairs: {len(ne_pairs)}")
    if not fe_pairs and not ne_pairs:
        sys.exit("no fixture wavs available — run git lfs pull first")

    reports: list[ConfigReport] = []
    for cfg in configs:
        print(f"\n== {cfg.name} ==")
        rep = ConfigReport(config=cfg)
        rep.farend  = score_farend(cfg, fe_pairs, tmp)
        rep.nearend = score_nearend(cfg, ne_pairs, tmp)
        s = rep.summary()
        print(json.dumps(s, indent=2))
        reports.append(rep)

    report_path = REPORTS_DIR / f"{name}.json"
    report_path.write_text(json.dumps([r.summary() for r in reports], indent=2))

    md = render_markdown(name, reports)
    (REPORTS_DIR / f"{name}.md").write_text(md)
    print(f"\nreport: {report_path}")
    return report_path


def render_markdown(name: str, reports: list[ConfigReport]) -> str:
    lines = [f"# mod_klear benchmark — `{name}`", ""]
    lines += ["## Far-end single-talk (ERLE — higher is better)", ""]
    lines += ["| config | n | ERLE dB | mic dBFS | out dBFS | RTF |",
              "|---|---:|---:|---:|---:|---:|"]
    for r in reports:
        s = r.summary()["farend_singletalk"]
        lines.append(
            f"| {r.config.name} | {s['n']} | "
            f"{s['erle_db_mean']:.2f} | {s['mic_rms_db_mean']:.1f} | "
            f"{s['out_rms_db_mean']:.1f} | {s['rtf_mean']:.4f} |")
    lines += ["", "## Near-end single-talk (preservation — higher is better)", ""]
    lines += ["| config | n | PESQ-wb | STOI | SI-SDR dB | RTF |",
              "|---|---:|---:|---:|---:|---:|"]
    for r in reports:
        s = r.summary()["nearend_singletalk"]
        lines.append(
            f"| {r.config.name} | {s['n']} | "
            f"{s['pesq_wb_mean']:.3f} | {s['stoi_mean']:.3f} | "
            f"{s['si_sdr_db_mean']:.2f} | {s['rtf_mean']:.4f} |")
    lines += ["", "## Pipeline latency", ""]
    lines += ["| config | algo delay ms |", "|---|---:|"]
    for r in reports:
        s = r.summary()
        lines.append(f"| {r.config.name} | {s['algo_delay_ms']} |")
    lines.append("")
    return "\n".join(lines)


def main(argv: Iterable[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default="baseline",
                    help="name for this run (used as report filename)")
    ap.add_argument("--max", type=int, default=6,
                    help="max pairs per scenario (limits wall time)")
    args = ap.parse_args(list(argv))
    run(args.run, DEFAULT_CONFIGS, args.max)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
