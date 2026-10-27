#!/usr/bin/env python3
"""Repeatable warm/cold Chatterbox Turbo benchmark for ARLO Mini.

Run this with the Chatterbox environment, for example:

  ./.chatterbox-venv/Scripts/python.exe tools/benchmark_voice.py \
      --reference voice/miss-minutes-2s-12s.wav --runs 5 \
      --output .hermes/voice-benchmark.json

The script never prints credentials, transcripts from a live service, or audio
contents. It uses fixed benchmark text unless --text-file is supplied.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import tempfile
import time
import wave
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]

DEFAULT_TEXTS = {
    "short": "Well now, how can I help?",
    "medium": (
        "Well now, that's a mighty fine question, and I'll get it sorted out "
        "for you in just a minute while we keep the important details in order."
    ),
    "long": (
        "Well now, here's the plan: we'll take this one step at a time, check "
        "the important details, and keep the whole thing moving along without "
        "making a bigger fuss than necessary. We'll confirm what the conversation "
        "needs, handle the smallest useful piece, and keep moving. If something "
        "is uncertain, I'll say so plainly instead of guessing. If the work gets "
        "tangled, we'll slow down and sort it out together, without losing the "
        "thread."
    ),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", default=os.getenv("CHATTERBOX_REFERENCE", ""))
    parser.add_argument("--device", default=os.getenv("CHATTERBOX_DEVICE", "auto"))
    parser.add_argument(
        "--max-gen-len", type=int,
        default=int(os.getenv("CHATTERBOX_MAX_GEN_LEN", "600")),
    )
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--text-file", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "voice-benchmark.json")
    parser.add_argument("--keep-audio", action="store_true")
    return parser.parse_args()


def load_texts(path: Path | None) -> dict[str, str]:
    if path is None:
        return DEFAULT_TEXTS.copy()
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        raise ValueError("--text-file is empty")
    return {"custom": text}


def wav_duration(path: Path) -> float:
    with wave.open(str(path), "rb") as handle:
        frames = handle.getnframes()
        rate = handle.getframerate()
    return frames / rate if rate else 0.0


def gpu_snapshot(torch: Any) -> dict[str, Any]:
    available = bool(torch.cuda.is_available())
    result: dict[str, Any] = {"cuda_available": available}
    if available:
        result.update({
            "cuda_device": torch.cuda.get_device_name(0),
            "allocated_bytes": int(torch.cuda.memory_allocated()),
            "reserved_bytes": int(torch.cuda.memory_reserved()),
        })
    return result


def normalize_wav(raw: Path, normalized: Path) -> float:
    started = time.perf_counter()
    subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(raw), "-ac", "2", "-ar", "16000",
            "-acodec", "pcm_s16le", str(normalized),
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=90,
    )
    return (time.perf_counter() - started) * 1000


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    def values(key: str) -> list[float]:
        return [float(row[key]) for row in rows if row.get(key) is not None]

    def stats(key: str) -> dict[str, float | None]:
        data = values(key)
        if not data:
            return {"median_ms": None, "min_ms": None, "max_ms": None}
        return {
            "median_ms": statistics.median(data),
            "min_ms": min(data),
            "max_ms": max(data),
        }

    return {
        "runs": len(rows),
        "generation": stats("generation_ms"),
        "normalization": stats("normalize_ms"),
        "audio_duration": stats("audio_duration_s"),
        "rtf": stats("rtf"),
    }


def main() -> int:
    args = parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be at least 1")
    reference = Path(args.reference).expanduser()
    if not reference.is_file():
        raise SystemExit(f"Reference WAV does not exist: {reference}")

    import torch
    from chatterbox.tts_turbo import ChatterboxTurboTTS

    device = args.device
    if device == "auto":
        device = "cuda" if torch.cuda.is_available() else "cpu"

    load_started = time.perf_counter()
    model = ChatterboxTurboTTS.from_pretrained(device=device, nano=False)
    model.t3.inference_turbo = _limited_inference(
        model.t3.inference_turbo, args.max_gen_len
    )
    model.prepare_conditionals(str(reference))
    preload_ms = (time.perf_counter() - load_started) * 1000
    cold_snapshot = gpu_snapshot(torch)

    texts = load_texts(args.text_file)
    all_results: dict[str, Any] = {
        "schema": 1,
        "reference": str(reference),
        "device": device,
        "max_gen_len": args.max_gen_len,
        "preload_ms": preload_ms,
        "preload_gpu": cold_snapshot,
        "first_audio_chunk_ms": None,
        "first_audio_chunk_note": (
            "The installed Chatterbox generate() API returns a complete tensor; "
            "no chunk timestamp is available."
        ),
        "benchmarks": {},
    }

    with tempfile.TemporaryDirectory(prefix="arlo-voice-benchmark-") as temp_name:
        temp_dir = Path(temp_name)
        for label, text in texts.items():
            rows: list[dict[str, Any]] = []
            for run in range(1, args.runs + 1):
                raw = temp_dir / f"{label}-{run}-raw.wav"
                normalized = temp_dir / f"{label}-{run}-normalized.wav"
                if torch.cuda.is_available():
                    torch.cuda.synchronize()
                started = time.perf_counter()
                wav = model.generate(text)
                if torch.cuda.is_available():
                    torch.cuda.synchronize()
                generation_ms = (time.perf_counter() - started) * 1000
                if wav.ndim == 1:
                    wav = wav.unsqueeze(0)
                import torchaudio
                torchaudio.save(str(raw), wav.detach().cpu(), model.sr)
                normalize_ms = normalize_wav(raw, normalized)
                duration = wav_duration(normalized)
                row = {
                    "run": run,
                    "generation_ms": generation_ms,
                    "normalize_ms": normalize_ms,
                    "audio_duration_s": duration,
                    "rtf": generation_ms / (duration * 1000) if duration else None,
                    "gpu": gpu_snapshot(torch),
                }
                rows.append(row)
                if args.keep_audio:
                    destination = args.output.parent / f"{label}-{run}.wav"
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(normalized.read_bytes())
            all_results["benchmarks"][label] = {
                "word_count": len(text.split()),
                "text_sha256": _sha256(text),
                "summary": summarize(rows),
                "runs": rows,
            }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(all_results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "output": str(args.output),
        "preload_ms": round(preload_ms, 1),
        "benchmarks": {
            label: value["summary"]
            for label, value in all_results["benchmarks"].items()
        },
    }, indent=2))
    return 0


def _limited_inference(original: Any, max_gen_len: int) -> Any:
    def limited(*args: Any, **kwargs: Any) -> Any:
        kwargs["max_gen_len"] = max_gen_len
        return original(*args, **kwargs)
    return limited


def _sha256(value: str) -> str:
    import hashlib
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


if __name__ == "__main__":
    raise SystemExit(main())
