import json
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bridge"))
import server


def test_chatterbox_backend_requires_a_reference_recording(monkeypatch, tmp_path):
    monkeypatch.setattr(server, "TTS_BACKEND", "chatterbox")
    monkeypatch.setattr(server, "CHATTERBOX_REFERENCE", "")

    with pytest.raises(RuntimeError, match="CHATTERBOX_REFERENCE"):
        server._run_tts("Hello there", tmp_path / "raw.wav", tmp_path)


def test_chatterbox_cpp_backend_invokes_native_cuda_cli(monkeypatch, tmp_path):
    binary = tmp_path / "tts-cli.exe"
    model = tmp_path / "t3.gguf"
    s3gen = tmp_path / "s3gen.gguf"
    profile = tmp_path / "profile"
    for path in (binary, model, s3gen):
        path.write_bytes(b"placeholder")
    profile.mkdir()
    raw = tmp_path / "raw.wav"
    calls = {}

    class Result:
        stdout = ""
        stderr = (
            "BENCH: T3_LOAD_MS=1200\\n"
            "BENCH: S3GEN_INFER_MS=700 AUDIO_MS=1840\\n"
            "BENCH: T3_INFER_MS=2600\\n"
        )

    def fake_run(command, **kwargs):
        calls["command"] = command
        calls["kwargs"] = kwargs
        raw.write_bytes(b"RIFF" + b"0" * 50)
        return Result()

    monkeypatch.setattr(server, "CHATTERBOX_CPP_BINARY", str(binary))
    monkeypatch.setattr(server, "CHATTERBOX_CPP_MODEL", str(model))
    monkeypatch.setattr(server, "CHATTERBOX_CPP_S3GEN", str(s3gen))
    monkeypatch.setattr(server, "CHATTERBOX_CPP_PROFILE", str(profile))
    monkeypatch.setattr(server.subprocess, "run", fake_run)

    server.run_chatterbox_cpp_tts("Well now, hello.", raw, tmp_path)

    assert calls["command"][0] == str(binary)
    assert "--n-gpu-layers" in calls["command"]
    assert calls["command"][calls["command"].index("--n-gpu-layers") + 1] == "99"
    assert "--stream-chunk-tokens" in calls["command"]
    assert calls["command"][calls["command"].index("--stream-chunk-tokens") + 1] == "50"
    assert "--stream-first-chunk-tokens" in calls["command"]
    assert calls["command"][calls["command"].index("--stream-first-chunk-tokens") + 1] == "50"
    assert calls["kwargs"]["check"] is True
    assert calls["kwargs"]["cwd"] == str(tmp_path)
    assert raw.read_bytes()[:4] == b"RIFF"


def test_chatterbox_cpp_live_command_uses_incremental_input_and_raw_stdout(
    monkeypatch, tmp_path
):
    binary = tmp_path / "tts-cli.exe"
    model = tmp_path / "t3.gguf"
    s3gen = tmp_path / "s3gen.gguf"
    profile = tmp_path / "profile"
    for path in (binary, model, s3gen):
        path.write_bytes(b"placeholder")
    profile.mkdir()

    monkeypatch.setattr(server, "CHATTERBOX_CPP_BINARY", str(binary))
    monkeypatch.setattr(server, "CHATTERBOX_CPP_MODEL", str(model))
    monkeypatch.setattr(server, "CHATTERBOX_CPP_S3GEN", str(s3gen))
    monkeypatch.setattr(server, "CHATTERBOX_CPP_PROFILE", str(profile))

    command = server.build_chatterbox_cpp_command(
        text=None,
        output="-",
        input_file=tmp_path / "live-input.txt",
        eof_marker="__ARLO_EOF__",
    )

    assert command[0] == str(binary)
    assert "--input-file" in command
    assert str(tmp_path / "live-input.txt") in command
    assert "--input-eof-marker" in command
    assert "__ARLO_EOF__" in command
    assert command[command.index("--out") + 1] == "-"
    assert "--input-by-line" in command
    assert command[command.index("--stream-chunk-tokens") + 1] == "50"
    assert command[command.index("--stream-first-chunk-tokens") + 1] == "50"


def test_hermes_can_use_a_bot_soul_file(monkeypatch, tmp_path):
    soul = tmp_path / "SOUL.md"
    soul.write_text("Speak with a gentle Texas rhythm.", encoding="utf-8")
    monkeypatch.setattr(server, "HERMES_SOUL_FILE", str(soul))

    prompt = server.get_system_prompt()

    assert "Speak with a gentle Texas rhythm." in prompt
    assert "keep spoken replies under 60 words" in prompt


def test_stt_worker_mode_keeps_faster_whisper_out_of_chatterbox_process(
    monkeypatch, tmp_path
):
    calls = {}

    class WorkerStdin:
        def write(self, value):
            calls["request"] = value

        def flush(self):
            calls["flushed"] = True

    class WorkerStdout:
        def readline(self):
            return '{"transcript":"Hello from the worker"}\n'

    class WorkerProcess:
        stdin = WorkerStdin()
        stdout = WorkerStdout()
        returncode = None

        def poll(self):
            return self.returncode

        def terminate(self):
            calls["terminated"] = True

    def fake_popen(command, **kwargs):
        calls.update(command=command, kwargs=kwargs)
        return WorkerProcess()

    monkeypatch.setattr(server, "STT_WORKER_PYTHON", "C:/worker/python.exe")
    monkeypatch.setattr(server.subprocess, "Popen", fake_popen)
    monkeypatch.setattr(server, "stt_worker_process", None)
    input_path = tmp_path / "input.wav"

    assert server.speech_to_text(input_path) == "Hello from the worker"
    assert calls["command"][0] == "C:/worker/python.exe"
    assert calls["command"][1].replace("\\", "/").endswith("bridge/stt_worker.py")
    assert calls["command"][2] == "--serve"
    assert __import__("json").loads(calls["request"])["path"] == str(input_path)
    assert calls["flushed"] is True


def test_hermes_request_limits_default_spoken_answer_length(monkeypatch):
    captured = {}

    class Response:
        def raise_for_status(self):
            pass

        def json(self):
            return {"choices": [{"message": {"content": "A short answer."}}]}

    def fake_post(url, **kwargs):
        captured.update(url=url, kwargs=kwargs)
        return Response()

    monkeypatch.setattr(server, "HERMES_KEY", "test-key")
    monkeypatch.setattr(server, "HERMES_URL", "http://hermes.test")
    monkeypatch.setattr(server, "MOCK_HERMES", False)
    monkeypatch.setattr(server, "history", [])
    monkeypatch.setattr(server.requests, "post", fake_post)

    assert server.ask_hermes("What can you do?") == "A short answer."
    body = captured["kwargs"]["json"]
    assert body["max_tokens"] == 96
    assert "under 60 words" in body["messages"][0]["content"]


def test_sentence_streaming_does_not_split_at_commas(monkeypatch):
    class Response:
        def raise_for_status(self):
            pass

        def iter_lines(self, decode_unicode=True):
            event = {
                "choices": [{
                    "delta": {
                        "content": (
                            "Well now, sugar, that's a mighty fine question. "
                            "Give me just a minute, and we'll get it sorted out."
                        )
                    }
                }]
            }
            return iter([
                f"data: {json.dumps(event)}",
                'data: [DONE]',
            ])

    monkeypatch.setattr(server, "HERMES_KEY", "test-key")
    monkeypatch.setattr(server, "HERMES_URL", "http://hermes.test")
    monkeypatch.setattr(server, "MOCK_HERMES", False)
    monkeypatch.setattr(server, "CHATTERBOX_PHRASE_STREAMING", False)
    monkeypatch.setattr(server, "history", [])
    monkeypatch.setattr(server.requests, "post", lambda *args, **kwargs: Response())
    segments = []

    server.ask_hermes("What should I do?", on_sentence=segments.append)

    assert segments == [
        "Well now, sugar, that's a mighty fine question.",
        "Give me just a minute, and we'll get it sorted out.",
    ]


def test_hermes_stream_records_first_event_content_phrase_and_usage(monkeypatch):
    class Response:
        def raise_for_status(self):
            pass

        def iter_lines(self, decode_unicode=True):
            events = [
                {
                    "model": "hermes-agent",
                    "choices": [{"delta": {"reasoning_content": "thinking"}}],
                },
                {
                    "model": "hermes-agent",
                    "choices": [{
                        "delta": {
                            "tool_calls": [{"id": "call-1", "type": "function"}]
                        }
                    }],
                },
                {
                    "model": "hermes-agent",
                    "choices": [{"delta": {"content": "A short answer. "}}],
                },
                {
                    "model": "hermes-agent",
                    "choices": [{"delta": {"content": "A second sentence."}}],
                    "usage": {
                        "prompt_tokens": 31,
                        "completion_tokens": 9,
                        "total_tokens": 40,
                    },
                },
            ]
            return iter([
                *(f"data: {json.dumps(event)}" for event in events),
                "data: [DONE]",
            ])

    monkeypatch.setattr(server, "HERMES_KEY", "test-key")
    monkeypatch.setattr(server, "HERMES_URL", "http://hermes.test")
    monkeypatch.setattr(server, "MOCK_HERMES", False)
    monkeypatch.setattr(server, "history", [])
    monkeypatch.setattr(server.requests, "post", lambda *args, **kwargs: Response())
    server._timing_context.metrics = {}
    try:
        segments = []
        result = server.ask_hermes("What can you do?", on_sentence=segments.append)
        metrics = server._timing_context.metrics
    finally:
        server._timing_context.metrics = None

    assert result == "A short answer. A second sentence."
    assert segments == ["A short answer.", "A second sentence."]
    assert metrics["hermes_request_start_ms"] >= 0
    assert metrics["hermes_first_sse_event_ms"] >= 0
    assert metrics["hermes_first_content_token_ms"] >= 0
    assert metrics["hermes_first_phrase_ms"] >= 0
    assert metrics["hermes_first_phrase_words"] == 3
    assert metrics["hermes_reasoning_chars"] == len("thinking")
    assert metrics["hermes_prompt_tokens"] == 31
    assert metrics["hermes_completion_tokens"] == 9
    assert metrics["hermes_total_tokens"] == 40
    assert metrics["hermes_model"] == "hermes-agent"
    assert metrics["hermes_tool_call_chunks"] == 1
    assert metrics["hermes_first_tool_call_ms"] >= 0


def test_chatterbox_backend_is_selected_by_bridge(monkeypatch, tmp_path):
    called = {}

    def fake_chatterbox(text, raw, directory):
        called.update(text=text, raw=raw, directory=directory)

    monkeypatch.setattr(server, "TTS_BACKEND", "chatterbox")
    monkeypatch.setattr(server, "run_chatterbox_tts", fake_chatterbox)

    server._run_tts("Hello there", tmp_path / "raw.wav", tmp_path)

    assert called == {
        "text": "Hello there",
        "raw": tmp_path / "raw.wav",
        "directory": tmp_path,
    }


def test_chatterbox_preload_warms_model_and_reference_conditionals(monkeypatch, tmp_path):
    calls = []

    class Model:
        def prepare_conditionals(self, reference):
            calls.append(reference)

    reference = tmp_path / "reference.wav"
    reference.write_bytes(b"RIFF")
    monkeypatch.setattr(server, "CHATTERBOX_REFERENCE", str(reference))
    monkeypatch.setattr(server, "_load_chatterbox_model", lambda: Model())

    server.preload_chatterbox()

    assert calls == [str(reference)]


def test_chatterbox_generation_has_a_configurable_token_limit(monkeypatch):
    calls = {}

    class T3:
        def inference_turbo(self, *args, **kwargs):
            calls.update(kwargs)
            return "tokens"

    class Model:
        t3 = T3()

    monkeypatch.setattr(server, "CHATTERBOX_MAX_GEN_LEN", 321)
    model = Model()
    server.limit_chatterbox_generation(model)

    assert model.t3.inference_turbo(text_tokens="text") == "tokens"
    assert calls["max_gen_len"] == 321


def test_chatterbox_loader_passes_nano_selection(monkeypatch):
    calls = {}

    class T3:
        def inference_turbo(self, *args, **kwargs):
            return "tokens"

    class Model:
        t3 = T3()

    class ChatterboxTurboTTS:
        @classmethod
        def from_pretrained(cls, **kwargs):
            calls.update(kwargs)
            return Model()

    class Torch:
        class cuda:
            @staticmethod
            def is_available():
                return True

    monkeypatch.setitem(__import__('sys').modules, 'torch', Torch)
    monkeypatch.setitem(
        __import__('sys').modules,
        'chatterbox.tts_turbo',
        type('Module', (), {'ChatterboxTurboTTS': ChatterboxTurboTTS}),
    )
    monkeypatch.setattr(server, 'CHATTERBOX_DEVICE', 'cuda')
    monkeypatch.setattr(server, 'CHATTERBOX_NANO', True)
    monkeypatch.setattr(server, 'chatterbox_model', None)

    server._load_chatterbox_model()

    assert calls == {'device': 'cuda', 'nano': True}
