"""One-shot Whisper worker for Chatterbox bridge isolation.

The Chatterbox process must not import faster-whisper because its native CUDA
libraries can conflict with PyTorch/cuDNN on Windows. This worker runs in the
project environment and returns one JSON transcript on stdout.
"""

from __future__ import annotations

import json
import os
import sys
from typing import Any
from pathlib import Path


HALLUCINATIONS = {
    "",
    "thank you",
    "thanks for watching",
    "you",
    "bye",
    "subtitles by the amara.org community",
}


def load_model() -> Any:
    from faster_whisper import WhisperModel

    return WhisperModel(
        os.getenv("STT_MODEL", "base.en"),
        device=os.getenv("STT_DEVICE", "auto"),
        compute_type=os.getenv("STT_COMPUTE_TYPE", "default"),
    )


def transcribe(path: Path, model: Any = None) -> str:
    if model is None:
        model = load_model()
    segments, _ = model.transcribe(
        str(path),
        language="en",
        beam_size=3,
        vad_filter=True,
        condition_on_previous_text=False,
    )
    transcript = " ".join(segment.text.strip() for segment in segments).strip()
    if transcript.lower().strip(" .!?") in HALLUCINATIONS:
        return ""
    return transcript[:1200]


def serve() -> None:
    """Keep Whisper loaded and transcribe newline-delimited JSON requests."""
    model = load_model()
    for line in sys.stdin:
        if not line.strip():
            continue
        request = json.loads(line)
        transcript = transcribe(Path(request["path"]), model)
        print(json.dumps({"transcript": transcript}), flush=True)


def main() -> None:
    if len(sys.argv) == 2 and sys.argv[1] == "--serve":
        serve()
        return
    if len(sys.argv) != 2:
        raise SystemExit("usage: stt_worker.py INPUT_WAV | --serve")
    print(json.dumps({"transcript": transcribe(Path(sys.argv[1]))}))


if __name__ == "__main__":
    main()
