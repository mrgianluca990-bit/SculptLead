#!/usr/bin/env python3
"""Build the Sculpt Lead acoustic bank from pinned, checksum-verified VSCO WAVs.

Build-time dependencies: Python 3.10+, NumPy and SciPy. No runtime dependency.
Usage: python tools/import_factory_samples.py --cache /path/to/vsco-cache
       python tools/import_factory_samples.py --verify

The two variants are real recorded tongue attacks spliced into one real sustain
recording. They are not two independently sampled sustained/legato performances.
See docs/SAMPLES.md for the exact loop and provenance contracts.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
from fractions import Fraction
from pathlib import Path
import sys
import urllib.request

import numpy as np
from scipy import signal
from scipy.io import wavfile


PROJECT = Path(__file__).resolve().parents[1]
RATE = 48000
GENERATOR_VERSION = "1.0.0"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x), dtype=np.float64) + 1.0e-20))


def db(x: float) -> float:
    return 20.0 * math.log10(max(1.0e-12, x))


def download_sources(config: dict, cache: Path, offline: bool = False) -> None:
    def one(item: tuple[str, dict]) -> None:
        name, record = item
        path = cache / name
        if not path.exists():
            if offline:
                raise FileNotFoundError(f"Source missing in offline mode: {path}")
            path.parent.mkdir(parents=True, exist_ok=True)
            request = urllib.request.Request(record["url"], headers={"User-Agent": "Sculpt-Lead-Asset-Builder/1.0"})
            with urllib.request.urlopen(request, timeout=90) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != record["sha256"]:
                raise ValueError(f"Downloaded source checksum mismatch: {name}")
            temporary = path.with_name(path.name + ".part")
            temporary.write_bytes(data)
            temporary.replace(path)
        if sha256(path) != record["sha256"]:
            raise ValueError(f"Cached source checksum mismatch: {name}")

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        list(executor.map(one, config["sources"].items()))


def read_mono(path: Path) -> tuple[np.ndarray, dict]:
    rate, audio = wavfile.read(path)
    dtype = audio.dtype
    if dtype.kind in "iu":
        if dtype.kind == "u":
            audio = (audio.astype(np.float64) - 128.0) / 128.0
        else:
            audio = audio.astype(np.float64) / float(2 ** (dtype.itemsize * 8 - 1))
    else:
        audio = audio.astype(np.float64)
    channels = 1 if audio.ndim == 1 else audio.shape[1]
    downmix = "mono"
    if channels > 1:
        mean = np.mean(audio, axis=1)
        channel_rms = np.sqrt(np.mean(audio * audio, axis=0))
        # Avoid cancelling a genuine polarity error in a source recording.
        if rms(mean) < float(np.max(channel_rms)) * 0.25:
            choice = int(np.argmax(channel_rms))
            audio = audio[:, choice]
            downmix = f"channel {choice + 1}; stereo mean had severe cancellation"
        else:
            audio = mean
            downmix = "arithmetic stereo mean"
    if rate != RATE:
        factor = math.gcd(int(rate), RATE)
        audio = signal.resample_poly(audio, RATE // factor, int(rate) // factor)
    # Low-cut only: retain the recorded breath and upper harmonics.
    audio = signal.sosfilt(signal.butter(2, 20.0, btype="highpass", fs=RATE, output="sos"), audio)
    threshold = max(1.0e-7, float(np.max(np.abs(audio))) * 0.0015)
    active = np.flatnonzero(np.abs(audio) > threshold)
    trim = max(0, int(active[0]) - int(0.003 * RATE)) if active.size else 0
    audio = audio[trim:]
    # Only the beginning and first few seconds are needed for this compact bank.
    audio = audio[: int(4.0 * RATE)]
    return audio, {"originalSampleRate": int(rate), "originalChannels": channels,
                   "downmix": downmix, "leadingSilenceTrimmedFrames48k": trim}


def estimate_tuning(audio: np.ndarray, root: int, attack: bool = False) -> tuple[float, float]:
    """Find the local harmonic spectral maximum around the SFZ key centre.

    Returning a static offset does not remove the performer's natural fluctuations.
    A broad search also exposes the known filename/key-centre octave convention.
    """
    if attack:
        left, right = int(0.055 * RATE), min(len(audio), int(0.32 * RATE))
    else:
        left, right = int(0.55 * RATE), min(len(audio), int(1.75 * RATE))
    segment = audio[left:right]
    if len(segment) < RATE // 25 or rms(segment) < 1.0e-6:
        return 0.0, 0.0
    segment = segment - np.mean(segment)
    nfft = 2 ** max(17, int(math.ceil(math.log2(len(segment)))) + 2)
    spectrum = np.abs(np.fft.rfft(segment * np.hanning(len(segment)), n=nfft))
    frequencies = np.fft.rfftfreq(nfft, 1.0 / RATE)
    fundamental = 440.0 * 2.0 ** ((root - 69) / 12.0)
    cents = np.linspace(-160.0, 160.0, 1281)
    candidates = fundamental * 2.0 ** (cents / 1200.0)
    score = np.zeros_like(candidates)
    count = min(8, max(1, int(12000 / fundamental)))
    for harmonic in range(1, count + 1):
        partial = np.interp(candidates * harmonic, frequencies, spectrum)
        score += partial / math.sqrt(harmonic)
    best = int(np.argmax(score))
    offset = float(cents[best])
    confidence = float(score[best] / (np.median(score) + 1.0e-20))
    return offset, confidence


def pitch_shift(audio: np.ndarray, semitones: float) -> np.ndarray:
    if abs(semitones) < 0.004:
        return audio
    ratio = Fraction(2.0 ** (-semitones / 12.0)).limit_denominator(5000)
    return signal.resample_poly(audio, ratio.numerator, ratio.denominator)


def choose_loop(audio: np.ndarray) -> tuple[int, int, float]:
    """Match the last 20 ms to the 20 ms immediately BEFORE loopStart.

    Playback blends the last N samples with [loopStart-N, loopStart) then wraps
    exactly to loopStart. The actual loop period is loopEnd-loopStart.
    """
    width = int(0.020 * RATE)
    available = min(len(audio) - 8, int(2.65 * RATE))
    if available < int(1.7 * RATE):
        raise ValueError("Sustain is too short for the factory-loop recipe")
    best = None
    for start in np.linspace(int(0.88 * RATE), int(1.18 * RATE), 13).astype(int):
        earliest = start + int(0.72 * RATE)
        latest = min(available, start + int(1.35 * RATE))
        if latest <= earliest:
            continue
        reference = audio[start - width:start]
        search = audio[earliest - width:latest]
        # valid dot products correspond to candidate exclusive loop ends.
        correlation = signal.correlate(search, reference, mode="valid", method="fft")
        integral = np.r_[0.0, np.cumsum(search * search)]
        energies = np.maximum(1.0e-20, integral[width:] - integral[:-width])
        reference_energy = float(np.dot(reference, reference)) + 1.0e-20
        correlation /= np.sqrt(energies * reference_energy)
        loudness_penalty = np.abs(np.log(np.sqrt(energies / reference_energy)))
        scores = correlation - 0.5 * loudness_penalty
        index = int(np.argmax(scores))
        end = earliest + index
        score = float(scores[index])
        if best is None or score > best[0]:
            best = (score, int(start), int(end), float(correlation[index]))
    assert best is not None
    return best[1], best[2], best[3]


def make_attack_variant(sustain: np.ndarray, attack: np.ndarray, root: int,
                        loop_start: int, loop_end: int) -> tuple[np.ndarray, int, int, dict]:
    envelope = np.sqrt(np.maximum(0.0, signal.convolve(attack * attack, np.ones(288) / 288.0, mode="same")))
    peak_at = int(np.argmax(envelope[: min(len(envelope), int(0.18 * RATE))]))
    fade_start = int(np.clip(peak_at + int(0.010 * RATE), int(0.055 * RATE), int(0.130 * RATE)))
    fade_length = int(0.080 * RATE)
    if len(attack) < fade_start + fade_length:
        raise ValueError("Recorded staccato is too short for the attack splice")
    reference = attack[fade_start:fade_start + fade_length]
    frequency = 440.0 * 2.0 ** ((root - 69) / 12.0)
    radius = int(2.0 * RATE / frequency)
    centre = int(0.34 * RATE)
    left = max(0, centre - radius)
    right = centre + radius + fade_length
    def matching_body(first: int, last: int) -> np.ndarray:
        search = sustain[first:last]
        result = signal.correlate(search, reference, mode="valid", method="fft")
        integral = np.r_[0.0, np.cumsum(search * search)]
        energy = np.maximum(1.0e-20, integral[fade_length:] - integral[:-fade_length])
        ref_energy = float(np.dot(reference, reference)) + 1.0e-20
        return result / np.sqrt(energy * ref_energy)

    correlation = matching_body(left, right)
    # A brass attack can change pitch while the tongue settles. If the local
    # match is poor, look farther into the same recorded sustain for a closer
    # match, always finishing the splice before the pre-loop region.
    if float(np.max(correlation)) < 0.30:
        alternate_left = int(0.20 * RATE)
        alternate_end = min(int(0.70 * RATE), loop_start - fade_length - int(0.10 * RATE))
        alternate = matching_body(alternate_left, alternate_end + fade_length)
        if float(np.max(alternate)) > float(np.max(correlation)):
            left = alternate_left
            correlation = alternate
    best = int(np.argmax(correlation))
    body_at_fade = left + best
    offset = body_at_fade - fade_start
    if offset < 0:
        raise ValueError("Invalid splice offset")
    body = sustain[offset:loop_end + 4].copy()
    body_rms = rms(body[fade_start:fade_start + fade_length])
    attack_gain = body_rms / rms(reference)
    attack_part = attack[:fade_start + fade_length] * attack_gain
    body[:fade_start] = attack_part[:fade_start]
    phase = np.linspace(0.0, 1.0, fade_length, endpoint=True)
    blend = phase * phase * (3.0 - 2.0 * phase)
    # The recorded attacks are not all identical in spectrum or breath noise.
    # Compensate their measured correlation so a low-correlation splice does
    # not briefly lose up to 3 dB; correlated waveforms are not boosted.
    rho = float(np.clip(correlation[best], 0.0, 1.0))
    compensation = 1.0 / np.sqrt((1.0 - blend) ** 2 + blend ** 2 + 2.0 * rho * blend * (1.0 - blend))
    body[fade_start:fade_start + fade_length] = (
        attack_part[fade_start:fade_start + fade_length] * (1.0 - blend)
        + body[fade_start:fade_start + fade_length] * blend) * compensation
    info = {"method": "recorded staccato attack into recorded sustain",
            "spliceStartFrame": fade_start, "spliceLengthFrames": fade_length,
            "sustainSourceOffsetFrames": offset, "attackGainDb": round(db(attack_gain), 3),
            "spliceCorrelation": round(float(correlation[best]), 6),
            "spliceEnergyCompensationDb": round(db(float(np.max(compensation))), 3)}
    return body, loop_start - offset, loop_end - offset, info


def write_pcm16(path: Path, audio: np.ndarray, seed: int) -> None:
    audio = audio.copy()
    # A tiny ramp suppresses any residual source edit; the acoustic onset remains.
    fade = min(48, len(audio))
    audio[:fade] *= np.linspace(0.0, 1.0, fade)
    generator = np.random.default_rng(seed)
    dither = (generator.random(len(audio)) - generator.random(len(audio))) / 65536.0
    pcm = np.rint(np.clip(audio + dither, -1.0, 32767.0 / 32768.0) * 32768.0).astype(np.int16)
    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(path, RATE, pcm)


def prepare_lead_onset(audio: np.ndarray, start: int, end: int) -> tuple[np.ndarray, int, int, dict]:
    """Keep the recorded transient, shortening excessive very quiet pre-roll.

    A 12 ms context precedes the first 5 ms-RMS crossing of 10% sustain RMS.
    Gradual natural attacks which are already audible are deliberately retained.
    """
    sustain_rms = rms(audio[start:end])
    envelope = np.sqrt(np.maximum(0.0, signal.convolve(
        audio[:int(0.4 * RATE)] ** 2, np.ones(240) / 240.0, mode="same")))
    active = np.flatnonzero(envelope >= sustain_rms * 0.1)
    onset = int(active[0]) if active.size else 0
    trimmed = max(0, onset - int(0.012 * RATE)) if onset > int(0.040 * RATE) else 0
    body_active = np.flatnonzero(envelope >= sustain_rms * 0.5)
    body_onset = int(body_active[0]) if body_active.size else len(envelope)
    return audio[trimmed:], start - trimmed, end - trimmed, {
        "quietPreRollTrimFrames": trimmed,
        "onset10PercentSustainRmsMs": round((onset - trimmed) * 1000 / RATE, 3),
        "onset50PercentSustainRmsMs": round((body_onset - trimmed) * 1000 / RATE, 3)}


def build(config: dict, cache: Path, output: Path) -> dict:
    regions = []
    diagnostics = []
    # Source decoding and tuning are shared for attacks reused by nearby roots.
    decoded: dict[str, tuple[np.ndarray, dict]] = {}
    tuned: dict[tuple[str, int, bool], tuple[np.ndarray, dict]] = {}

    def get_source(name: str, source_root: int, is_attack: bool) -> tuple[np.ndarray, dict]:
        key = (name, source_root, is_attack)
        if key not in tuned:
            if name not in decoded:
                decoded[name] = read_mono(cache / name)
            raw, info = decoded[name]
            cents, confidence = estimate_tuning(raw, source_root, is_attack)
            # Short, noisy staccatos can be unreliable: do not apply a dubious shift.
            correction = cents if confidence >= (1.12 if is_attack else 1.25) and abs(cents) < 155 else 0.0
            prepared = pitch_shift(raw, -correction / 100.0)
            tuned[key] = (prepared, {**info, "estimatedTuningCents": round(cents, 3),
                                    "tuningConfidence": round(confidence, 4),
                                    "appliedStaticTuningCents": round(-correction, 3)})
        return tuned[key]

    for number, spec in enumerate(config["instruments"]):
        inst = spec["instrument"]
        root = spec["rootNote"]
        layer = spec["velocityLayer"]
        sustain, sustain_info = get_source(spec["sustain"], root, False)
        loop_start, loop_end, correlation = choose_loop(sustain)
        gain = spec["targetRms"] / rms(sustain[loop_start:loop_end])
        sustain = sustain * gain
        prepared = []
        attacks = spec["attacks"]
        if not attacks:
            prepared.append((sustain[:loop_end + 4].copy(), loop_start, loop_end,
                             {"method": "original recorded sustain attack", "sourceAttack": None}))
        for attack_spec in attacks:
            name = attack_spec["path"]
            attack, attack_info = get_source(name, attack_spec["rootNote"], True)
            transposition = root - attack_spec["rootNote"]
            attack = pitch_shift(attack, transposition)
            waveform, start, end, splice = make_attack_variant(sustain, attack, root, loop_start, loop_end)
            splice.update({"sourceAttack": name, "attackTranspositionSemitones": transposition,
                           "sourceRecordedRoundRobin": attack_spec["sourceRoundRobin"],
                           "sourceAttackProcessing": attack_info})
            prepared.append((waveform, start, end, splice))
        peak = max(float(np.max(np.abs(x[0]))) for x in prepared)
        # One gain for the entire root/dynamic pair preserves RR sustain identity.
        safety_gain = min(1.0, 0.89 / max(1.0e-12, peak))
        for rr, (audio, start, end, splice) in enumerate(prepared):
            audio *= safety_gain
            audio, start, end, onset_info = prepare_lead_onset(audio, start, end)
            splice.update(onset_info)
            if "spliceStartFrame" in splice:
                splice["spliceStartFrame"] -= onset_info["quietPreRollTrimFrames"]
                splice["sustainSourceOffsetFrames"] += onset_info["quietPreRollTrimFrames"]
            filename = f"{inst}/{inst}_m{root:03d}_v{layer}_rr{rr}.wav"
            path = output / filename
            seed = int(hashlib.sha256(filename.encode()).hexdigest()[:8], 16)
            write_pcm16(path, audio, seed)
            record = {
                "instrument": inst, "rootNote": root, "velocityLayer": layer,
                "velocityCentre": spec["velocityCentre"], "roundRobin": rr,
                "file": filename, "sampleRate": RATE, "loopStart": start,
                "loopEnd": end, "gainDb": 0.0,
                "sha256": sha256(path), "frameCount": len(audio),
                "sustainRmsDbFS": round(db(rms(audio[start:end])), 3),
                "peakDbFS": round(db(float(np.max(np.abs(audio)))), 3),
                "provenance": {
                    "sourceSustain": spec["sustain"], "sourceSustainSha256": config["sources"][spec["sustain"]]["sha256"],
                    "sourceSustainProcessing": sustain_info,
                    "sourceSustainDynamic": spec["sourceDynamic"],
                    "sustainGainDb": round(db(gain * safety_gain), 3),
                    "loopPreRollCorrelation": round(correlation, 6), **splice}
            }
            if splice.get("sourceAttack"):
                record["provenance"]["sourceAttackSha256"] = config["sources"][splice["sourceAttack"]]["sha256"]
            regions.append(record)
        diagnostics.append({"instrument": inst, "root": root, "layer": layer,
                            "loopCorrelation": round(correlation, 4),
                            "sustainTuningCents": sustain_info["estimatedTuningCents"],
                            "peakSafetyGainDb": round(db(safety_gain), 3)})
        if number % 10 == 0:
            print(f"Prepared {number + 1}/{len(config['instruments'])} root/dynamic sets", flush=True)
    manifest = {"schema": 1, "name": "Sculpt Lead Factory", "version": "0.1.0",
                "generatorVersion": GENERATOR_VERSION,
                "upstream": config["upstream"],
                "loopPlayback": {"endIsExclusive": True, "crossfade": "tail to pre-loop, then wrap exactly to loopStart",
                                 "crossfadeMilliseconds": 20, "curve": "smoothstep linear amplitude"},
                "roundRobinDescription": "Recorded staccato attack variants share one recorded sustain per pitch/dynamic. Flute MIDI60/64 use one original sustain attack.",
                "regions": regions}
    output.mkdir(parents=True, exist_ok=True)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + "\n")
    (output / "build-report.json").write_text(json.dumps({"generatorVersion": GENERATOR_VERSION, "sets": diagnostics}, indent=2) + "\n")
    return manifest


def verify(output: Path) -> dict:
    manifest = json.loads((output / "manifest.json").read_text())
    names: set[str] = set()
    max_join = 0.0
    for region in manifest["regions"]:
        path = output / region["file"]
        if path.name in names:
            raise ValueError(f"Non-unique factory basename: {path.name}")
        names.add(path.name)
        if sha256(path) != region["sha256"]:
            raise ValueError(f"Prepared checksum mismatch: {path}")
        rate, pcm = wavfile.read(path)
        if rate != RATE or pcm.dtype != np.int16 or pcm.ndim != 1:
            raise ValueError(f"Invalid factory WAV format: {path}")
        if not (int(0.02 * RATE) < region["loopStart"] < region["loopEnd"] < len(pcm)):
            raise ValueError(f"Invalid loop bounds: {path}")
        if region["loopEnd"] - region["loopStart"] < int(0.7 * RATE):
            raise ValueError(f"Unexpected short loop: {path}")
        if np.max(np.abs(pcm.astype(np.int32))) >= 32767:
            raise ValueError(f"Clipped factory WAV: {path}")
        x = pcm.astype(np.float64) / 32768.0
        start, end = region["loopStart"], region["loopEnd"]
        width = min(960, (end - start) // 4, start)
        phase = np.arange(width, dtype=np.float64) / width
        fade = phase * phase * (3.0 - 2.0 * phase)
        crossfade = x[end - width:end] * (1.0 - fade) + x[start - width:start] * fade
        join = abs(float(x[start] - crossfade[-1]))
        natural_step = abs(float(x[start] - x[start - 1]))
        max_join = max(max_join, abs(join - natural_step))
        # The seam should differ from the natural waveform's own adjacent step
        # only by the tiny final smoothstep residual, not a full discontinuity.
        if abs(join - natural_step) > 1.0e-4:
            raise ValueError(f"Unexpected loop seam discontinuity: {path}")
    summary = {"regions": len(names), "bytes": sum((output / r["file"]).stat().st_size for r in manifest["regions"]),
               "maxLoopJoinResidual": max_join,
               "instruments": sorted(set(r["instrument"] for r in manifest["regions"]))}
    if set(summary["instruments"]) != {"clarinet", "flute", "trombone", "trumpet"}:
        raise ValueError("Factory bank is missing an acoustic instrument")
    print(json.dumps(summary, indent=2))
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sources", type=Path, default=PROJECT / "tools/factory_sources.json")
    parser.add_argument("--cache", type=Path, default=PROJECT / ".sample-cache")
    parser.add_argument("--output", type=Path, default=PROJECT / "Samples")
    parser.add_argument("--offline", action="store_true", help="Require every pinned source already in the cache")
    parser.add_argument("--verify", action="store_true", help="Verify existing output without downloading or rebuilding")
    parser.add_argument("--download-only", action="store_true")
    args = parser.parse_args()
    if args.verify:
        verify(args.output)
        return
    config = json.loads(args.sources.read_text())
    download_sources(config, args.cache, args.offline)
    if args.download_only:
        return
    build(config, args.cache, args.output)
    verify(args.output)


if __name__ == "__main__":
    main()
