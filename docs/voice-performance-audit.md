# ARLO Mini voice-performance audit

Date: 2026-10-26
Branch: `feature/georgia-voice-model`

## Scope

This audit covers the existing ARLO Mini path before further optimization. It preserves the active Chatterbox Turbo voice reference, sentence streaming, persistent serial workflow, and K10 playback implementation.

## Current path

```text
K10 VAD capture
  -> POST /turn (PCM WAV over the trusted LAN)
  -> persistent external faster-whisper worker
  -> Hermes Agent `/v1/chat/completions` (SSE when streaming)
  -> sentence buffer
  -> persistent Chatterbox Turbo + cached reference conditionals
  -> FFmpeg normalization to 16 kHz, 16-bit, stereo PCM WAV
  -> ordered K10 segment polling/download
  -> SD-card write and duration-based playback
```

## Repository state inspected

- Active branch: `feature/georgia-voice-model`.
- Working tree contains the existing voice and firmware changes; this work does not create a new branch or discard them.
- `bridge/server.py` keeps the Chatterbox model resident in `chatterbox_model` and serializes access with `chatterbox_lock`.
- `bridge/server.py` calls `prepare_conditionals()` only when the configured reference path changes. This is the existing conditioning cache and must remain the voice source of truth.
- `bridge/stt_worker.py` keeps faster-whisper in a separate process, avoiding the known Windows CUDA/cuDNN conflict with Chatterbox.
- Hermes SSE sentence callbacks already publish ordered audio segments before the final answer is complete.
- Sentence mode is the default. Commas do not end a segment unless the explicitly experimental phrase mode is enabled.
- The firmware polls `segments_ready`, downloads segments in order, waits from the WAV duration, and keeps the speaking face between segments.

## Likely bottlenecks

1. **Warm Chatterbox generation** is the dominant neural cost after preload. `CHATTERBOX_MAX_GEN_LEN` limits runaway turns but does not make Turbo a sub-second synthesizer.
2. **Hermes response time** contributes before the first complete sentence is available. The bridge uses SSE, so first-token and first-segment timing need to be distinguished.
3. **TTS and FFmpeg are currently measured together in the existing `tts_ms` log.** The instrumentation update will split generation, normalization, first-token, and first-audio-ready timings without changing the audio path.
4. **K10 transfer and playback are separate from bridge generation.** A bridge `ready` event means generation is complete, not that the device has finished speaking.
5. **Cold startup** is intentionally paid at bridge startup when `CHATTERBOX_PRELOAD=true`; it should be reported separately from warm-turn latency.

## Existing protections to preserve

- Turbo remains the default production backend because it preserves the selected Miss Minutes-inspired voice better than the tested alternatives.
- The active reference is the selected private 10-second mono WAV. It is not replaced by a generic Southern voice.
- STT remains isolated from Chatterbox.
- K10 output remains 16 kHz, 16-bit, stereo WAV.
- Audio segments remain sentence-sized unless phrase streaming is explicitly enabled.
- No bridge restart or serial-monitor shutdown should occur while the K10 is consuming queued audio.

## Measurement plan

The repeatable benchmark in `tools/benchmark_voice.py` records:

- preload/model-conditioning time;
- warm generation time;
- generated duration and real-time factor;
- FFmpeg normalization time;
- CUDA availability and peak allocated/reserved VRAM when available;
- short, medium, and long fixed utterance lengths with configurable repeated runs.

The bridge timing update records first Hermes content, first audio-ready, total TTS generation, normalization, and end-to-end job timings. First-audio *chunk* timing is reported as unavailable unless the installed Chatterbox API exposes a streaming generator; the current `generate()` API returns a complete tensor.

## Baseline measurements

The repeatable benchmark was run five times per fixed text with the active
10-second mono reference, CUDA on an NVIDIA GeForce RTX 4070, and
`CHATTERBOX_MAX_GEN_LEN=600`. Preload plus reference conditioning took
`19,591 ms` in this run. The generated waveform API does not expose an audio
chunk stream, so first-audio-chunk timing is unavailable at the TTS API layer.

| Case | Words | Median generation | Median audio | Median RTF | Generation range |
|---|---:|---:|---:|---:|---:|
| Short | 6 | 2,953 ms | 2.16 s | 1.37 | 2,914–3,451 ms |
| Medium | 27 | 11,243 ms | 9.16 s | 1.23 | 10,329–11,406 ms |
| Long | 73 | 29,854 ms | 24.16 s | 1.24 | 29,464–29,946 ms |

FFmpeg normalization was small but measurable: approximately `92 ms`, `103 ms`, and `120 ms` median for short, medium, and long outputs respectively.
The warm Turbo path is therefore slower than real time for all three cases,
with neural generation, not WAV conversion, as the dominant bottleneck. These
results are a baseline, not a claim of speedup.

The raw JSON report is written to `.hermes/voice-benchmark.json`, which is
runtime-local and intentionally not a distributable voice artifact.

## Acceptance gates

An optimization is acceptable only if it passes the Python suite, compile checks, and (for firmware changes) the PlatformIO build. A TTS change also requires a fresh physical K10 exchange and a listening check against the same reference and text. No backend replacement or voice-reference change is justified by timing alone.
