#!/usr/bin/env python3
"""Benchmark PCM mouth-envelope normalization on local WAV files.

This tool measures the old whole-file peak normalization, the old fixed-divisor
8 KiB streaming path, and the adaptive 20 ms streaming path used by the K10
firmware. It decodes through ffmpeg so float and PCM WAVs are compared under
the same 16 kHz mono analysis contract. Audio contents are never printed.

Example:
    python tools/benchmark_pcm_amplitude.py \
        voice/chatterbox-miss-minutes-test.wav \
        voice/chatterbox-bridge-test/reply.wav
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
from collections import Counter
from pathlib import Path

PCM_RATE = 16_000
PCM_FACE_WINDOW_FRAMES = 320
PCM_FACE_WINDOW_MS = 20
PCM_FACE_NOISE_FLOOR = 96.0
PCM_FACE_MIN_NOISE_FLOOR = 32.0
PCM_FACE_NOISE_FLOOR_FALL = 0.10
PCM_FACE_NOISE_FLOOR_RISE = 0.003
PCM_FACE_INITIAL_PEAK = 512.0
PCM_FACE_PEAK_DECAY = 0.999
PCM_FACE_ATTACK = 0.55
PCM_FACE_RELEASE = 0.25
PCM_FACE_HEADROOM = 0.70
PCM_FACE_NOISE_GATE = 8


def decode_levels(path: Path) -> list[float]:
    raw = subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(path),
            "-f",
            "s16le",
            "-ac",
            "1",
            "-ar",
            str(PCM_RATE),
            "pipe:1",
        ],
        check=True,
        capture_output=True,
    ).stdout
    if len(raw) < 2:
        return []
    samples = [
        int.from_bytes(raw[offset : offset + 2], "little", signed=True)
        for offset in range(0, len(raw) - 1, 2)
    ]
    return [
        sum(abs(sample) for sample in samples[offset : offset + PCM_FACE_WINDOW_FRAMES])
        / len(samples[offset : offset + PCM_FACE_WINDOW_FRAMES])
        for offset in range(0, len(samples), PCM_FACE_WINDOW_FRAMES)
        if len(samples[offset : offset + PCM_FACE_WINDOW_FRAMES])
        >= PCM_FACE_WINDOW_FRAMES // 2
    ]


def mouth_frame(amplitude: int) -> int:
    if amplitude < 4:
        return 0
    if amplitude < 24:
        return 1
    if amplitude < 82:
        return 2
    if amplitude < 170:
        return 3
    return 4


def frame_distribution(amplitudes: list[int]) -> dict[str, object]:
    frames: list[int] = []
    previous = 0
    for amplitude in amplitudes:
        frame = mouth_frame(amplitude)
        if amplitude > previous + 5 and frame < 4:
            frame += 1
        if previous > amplitude + 5 and frame > 0:
            frame -= 1
        frames.append(frame)
        previous = amplitude
    return frame_counts(frames)


def frame_counts(frames: list[int]) -> dict[str, object]:
    counts = Counter(frames)
    total = len(frames) or 1
    return {
        "counts": [counts[index] for index in range(5)],
        "percent": [round(counts[index] * 100.0 / total, 2) for index in range(5)],
    }


def old_whole_file(levels: list[float]) -> list[int]:
    peak = max(levels, default=1.0)
    return [min(255, round(level * 255.0 / max(1.0, peak))) for level in levels]


def old_fixed_divisor(levels: list[float]) -> list[int]:
    return [min(255, int(level / 128.0)) for level in levels]


def adaptive_stream(levels: list[float]) -> list[int]:
    noise_floor = PCM_FACE_NOISE_FLOOR
    peak = PCM_FACE_INITIAL_PEAK
    smoothed = 0.0
    amplitudes: list[int] = []
    for level in levels:
        if level < noise_floor:
            noise_floor += (level - noise_floor) * PCM_FACE_NOISE_FLOOR_FALL
        else:
            noise_floor += (level - noise_floor) * PCM_FACE_NOISE_FLOOR_RISE
        noise_floor = max(PCM_FACE_MIN_NOISE_FLOOR, noise_floor)

        peak = max(level, peak * PCM_FACE_PEAK_DECAY)
        span = max(1.0, peak - noise_floor)
        normalized = max(
            0.0,
            min(255.0, (level - noise_floor) * 255.0 / span * PCM_FACE_HEADROOM),
        )
        if normalized < PCM_FACE_NOISE_GATE:
            normalized = 0.0
        smoothing = PCM_FACE_ATTACK if normalized >= smoothed else PCM_FACE_RELEASE
        smoothed += (normalized - smoothed) * smoothing
        amplitudes.append(round(max(0.0, min(255.0, smoothed))))
    return amplitudes


def percentiles(levels: list[float]) -> dict[str, float]:
    if not levels:
        return {"mean": 0.0, "p10": 0.0, "p50": 0.0, "p90": 0.0, "p99": 0.0, "peak": 0.0}
    ordered = sorted(levels)

    def percentile(value: float) -> float:
        position = (len(ordered) - 1) * value
        low = math.floor(position)
        high = math.ceil(position)
        if low == high:
            return ordered[low]
        return ordered[low] + (ordered[high] - ordered[low]) * (position - low)

    return {
        "mean": round(statistics.mean(levels), 2),
        "p10": round(percentile(0.10), 2),
        "p50": round(percentile(0.50), 2),
        "p90": round(percentile(0.90), 2),
        "p99": round(percentile(0.99), 2),
        "peak": round(max(levels), 2),
    }


def analyze(path: Path) -> dict[str, object]:
    levels = decode_levels(path)
    adaptive = adaptive_stream(levels)
    return {
        "file": path.as_posix(),
        "duration_s": round(len(levels) * PCM_FACE_WINDOW_FRAMES / PCM_RATE, 3),
        "windows": len(levels),
        "raw_mean_abs": percentiles(levels),
        "whole_file_peak_frames": frame_distribution(old_whole_file(levels)),
        "fixed_8k_divisor_frames": frame_distribution(old_fixed_divisor(levels)),
        "adaptive_stream_frames": frame_distribution(adaptive),
        "adaptive_amplitude": percentiles([float(value) for value in adaptive]),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = {
        "contract": {"sample_rate": PCM_RATE, "window_ms": PCM_FACE_WINDOW_MS},
        "adaptive_config": {
            "initial_noise_floor": PCM_FACE_NOISE_FLOOR,
            "min_noise_floor": PCM_FACE_MIN_NOISE_FLOOR,
            "noise_floor_fall": PCM_FACE_NOISE_FLOOR_FALL,
            "noise_floor_rise": PCM_FACE_NOISE_FLOOR_RISE,
            "initial_peak": PCM_FACE_INITIAL_PEAK,
            "peak_decay": PCM_FACE_PEAK_DECAY,
            "attack": PCM_FACE_ATTACK,
            "release": PCM_FACE_RELEASE,
            "headroom": PCM_FACE_HEADROOM,
            "noise_gate": PCM_FACE_NOISE_GATE,
        },
        "files": [analyze(path) for path in args.files],
    }
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
