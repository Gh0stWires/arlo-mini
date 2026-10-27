"""ARLO Mini voice bridge: K10 WAV -> local STT -> Hermes -> local TTS -> K10 WAV.

This is a single-user trusted-LAN prototype. Do not expose port 8765 to the public Internet.
"""

from __future__ import annotations

import os
import json
import platform
import queue
import re
import shutil
import subprocess
import tempfile
import threading
import time
import uuid
import wave
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import requests
from dotenv import load_dotenv
from fastapi import BackgroundTasks, FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse, StreamingResponse

load_dotenv(Path(__file__).with_name('.env'))
DEVICE_KEY = os.getenv('DEVICE_KEY', '')
HERMES_URL = os.getenv('HERMES_URL', 'http://127.0.0.1:8642/v1/chat/completions')
HERMES_KEY = os.getenv('HERMES_KEY', '')
HERMES_MODEL = os.getenv('HERMES_MODEL', 'hermes-agent')
HERMES_MAX_TOKENS = int(os.getenv('HERMES_MAX_TOKENS', '96'))
HERMES_PROVIDER = os.getenv('HERMES_PROVIDER', '')
HERMES_SOUL_FILE = os.getenv('HERMES_SOUL_FILE', '')
STT_MODEL = os.getenv('STT_MODEL', 'base.en')
STT_WORKER_PYTHON = os.getenv('STT_WORKER_PYTHON', '')
TTS_BACKEND = os.getenv('TTS_BACKEND', 'auto').lower()
CHATTERBOX_REFERENCE = os.getenv('CHATTERBOX_REFERENCE', '')
CHATTERBOX_DEVICE = os.getenv('CHATTERBOX_DEVICE', 'auto')
CHATTERBOX_MAX_GEN_LEN = int(os.getenv('CHATTERBOX_MAX_GEN_LEN', '600'))
CHATTERBOX_PRELOAD = os.getenv('CHATTERBOX_PRELOAD', 'true').lower() == 'true'
CHATTERBOX_NANO = os.getenv('CHATTERBOX_NANO', 'false').lower() == 'true'
CHATTERBOX_CPP_BINARY = os.getenv('CHATTERBOX_CPP_BINARY', '')
CHATTERBOX_CPP_MODEL = os.getenv('CHATTERBOX_CPP_MODEL', '')
CHATTERBOX_CPP_S3GEN = os.getenv('CHATTERBOX_CPP_S3GEN', '')
CHATTERBOX_CPP_PROFILE = os.getenv('CHATTERBOX_CPP_PROFILE', '')
CHATTERBOX_CPP_RUNTIME_PATHS = os.getenv('CHATTERBOX_CPP_RUNTIME_PATHS', '')
CHATTERBOX_CPP_THREADS = int(os.getenv('CHATTERBOX_CPP_THREADS', '4'))
CHATTERBOX_CPP_GPU_LAYERS = int(os.getenv('CHATTERBOX_CPP_GPU_LAYERS', '99'))
CHATTERBOX_CPP_N_PREDICT = int(os.getenv(
    'CHATTERBOX_CPP_N_PREDICT', str(CHATTERBOX_MAX_GEN_LEN)))
CHATTERBOX_CPP_SEED = int(os.getenv('CHATTERBOX_CPP_SEED', '42'))
CHATTERBOX_CPP_TIMEOUT = int(os.getenv('CHATTERBOX_CPP_TIMEOUT', '300'))
CHATTERBOX_CPP_VERBOSE = os.getenv('CHATTERBOX_CPP_VERBOSE', 'true').lower() == 'true'
CHATTERBOX_CPP_BATCH = os.getenv('CHATTERBOX_CPP_BATCH', 'true').lower() == 'true'
CHATTERBOX_CPP_LIVE_STREAM = os.getenv(
    'CHATTERBOX_CPP_LIVE_STREAM', 'false').lower() == 'true'
CHATTERBOX_CPP_MAX_SENTENCE_CHARS = int(os.getenv(
    'CHATTERBOX_CPP_MAX_SENTENCE_CHARS', '180'))
CHATTERBOX_CPP_STREAM_CHUNK_TOKENS = int(os.getenv(
    'CHATTERBOX_CPP_STREAM_CHUNK_TOKENS', '50'))
CHATTERBOX_CPP_STREAM_FIRST_CHUNK_TOKENS = int(os.getenv(
    'CHATTERBOX_CPP_STREAM_FIRST_CHUNK_TOKENS', '50'))
CHATTERBOX_PHRASE_STREAMING = os.getenv(
    'CHATTERBOX_PHRASE_STREAMING', 'false').lower() == 'true'
CHATTERBOX_MIN_PHRASE_WORDS = int(os.getenv('CHATTERBOX_MIN_PHRASE_WORDS', '5'))
CHATTERBOX_MAX_PHRASE_WORDS = int(os.getenv('CHATTERBOX_MAX_PHRASE_WORDS', '10'))
MOCK_HERMES = os.getenv('MOCK_HERMES', 'false').lower() == 'true'
MAX_UPLOAD = 2_000_000  # enough for a few seconds of PCM WAV
JOB_TTL = 20 * 60


@asynccontextmanager
async def bridge_lifespan(_app):
    if TTS_BACKEND == 'chatterbox' and CHATTERBOX_PRELOAD:
        try:
            started = time.perf_counter()
            preload_chatterbox()
            print(f'[ARLO BRIDGE] Chatterbox warmed in '
                  f'{(time.perf_counter() - started) * 1000:.0f} ms', flush=True)
        except Exception as exc:
            # Keep the server available so the request path can report the
            # same actionable error instead of failing health checks.
            print(f'[ARLO BRIDGE] Chatterbox preload failed: '
                  f'{type(exc).__name__}: {str(exc)[:240]}', flush=True)
    yield


app = FastAPI(title="ARLO Mini bridge", docs_url=None, redoc_url=None,
              lifespan=bridge_lifespan)


EXIT_PHRASES = frozenset({
    'stop',
    'goodbye',
    'talk to you later',
    'never mind',
    "that's all",
})


def is_conversation_end_phrase(text: str) -> bool:
    """Return True only for a standalone, normalized exit phrase."""
    normalized = re.sub(r"[^a-z0-9']+", ' ', text.lower()).strip()
    normalized = re.sub(r'\s+', ' ', normalized)
    return normalized in EXIT_PHRASES


def split_spoken_sentences(text: str) -> list[str]:
    """Split a short spoken answer without dropping its punctuation."""
    parts = [part.strip() for part in re.split(r'(?<=[.!?])\s+', text.strip())]
    return [part for part in parts if part] or [text.strip()]


def _word_count(text: str) -> int:
    return len(re.findall(r"\b[\w']+\b", text))


def _pop_spoken_segment(buffer: str, final: bool = False):
    """Return one natural phrase and the unconsumed stream buffer."""
    punctuation = re.finditer(r'(?P<mark>[.!?,;:])(?P<space>\s+|$)', buffer)
    for match in punctuation:
        candidate = buffer[:match.end('mark')].strip()
        mark = match.group('mark')
        if mark in '.!?' or _word_count(candidate) >= CHATTERBOX_MIN_PHRASE_WORDS:
            return candidate, buffer[match.end('space'):].lstrip()

    words = list(re.finditer(r"\b[\w']+\b", buffer))
    if len(words) >= CHATTERBOX_MAX_PHRASE_WORDS:
        boundary = words[CHATTERBOX_MAX_PHRASE_WORDS - 1].end()
        if boundary < len(buffer) and buffer[boundary].isspace():
            return buffer[:boundary].strip(), buffer[boundary:].lstrip()

    if final and buffer.strip():
        return buffer.strip(), ''
    return None, buffer


def consume_spoken_segments(buffer: str, final: bool = False):
    """Consume all currently complete natural phrases from a text buffer."""
    segments = []
    while buffer.strip():
        segment, remainder = _pop_spoken_segment(buffer, final=final)
        if segment is None:
            break
        segments.append(segment)
        buffer = remainder
        if not final and not buffer.strip():
            break
    return segments, buffer


def consume_spoken_sentences(buffer: str, final: bool = False):
    """Consume complete sentence units without treating commas as boundaries."""
    if final:
        stripped = buffer.strip()
        return (split_spoken_sentences(stripped) if stripped else []), ''
    parts = re.split(r'(?<=[.!?])\s+', buffer)
    pending = parts.pop()
    return [part.strip() for part in parts if part.strip()], pending


def consume_stream_segments(buffer: str, final: bool = False):
    if CHATTERBOX_PHRASE_STREAMING:
        return consume_spoken_segments(buffer, final=final)
    return consume_spoken_sentences(buffer, final=final)


def split_spoken_phrases(text: str) -> list[str]:
    """Split spoken text into short clause-sized segments for early playback."""
    segments, remainder = consume_spoken_segments(text.strip(), final=True)
    if remainder.strip():
        segments.append(remainder.strip())
    return segments or [text.strip()]


def split_spoken_segments(text: str) -> list[str]:
    if CHATTERBOX_PHRASE_STREAMING:
        return split_spoken_phrases(text)
    return split_spoken_sentences(text)


# No audio files are retained permanently; each job has its own temp directory.
@dataclass
class Job:
    id: str
    directory: tempfile.TemporaryDirectory
    state: str = 'queued'
    answer: str = ''
    transcript: str = ''
    error: str = ''
    audio_path: Optional[Path] = None
    audio_segments: list[Path] = field(default_factory=list)
    segments_ready: int = 0
    segments_total: int = 0
    stream_complete: bool = False
    pcm_stream_ready: bool = False
    pcm_stream_complete: bool = False
    pcm_stream_error: str = ''
    pcm_stream_claimed: bool = False
    pcm_stream_claim_lock: threading.Lock = field(default_factory=threading.Lock, repr=False)
    pcm_stream_session: object = field(default=None, repr=False)
    timings: dict[str, float] = field(default_factory=dict)
    cancel_requested: bool = False
    created: float = field(default_factory=time.time)



def _chatterbox_cpp_runtime(binary: Path, model: Path) -> tuple[dict[str, str], Path]:
    runtime_env = os.environ.copy()
    runtime_paths = [str(binary.parent), str(model.parent)]
    if CHATTERBOX_CPP_RUNTIME_PATHS:
        runtime_paths.extend(part for part in CHATTERBOX_CPP_RUNTIME_PATHS.split(os.pathsep) if part)
    runtime_env['PATH'] = os.pathsep.join(runtime_paths + [runtime_env.get('PATH', '')])
    cwd = Path(os.getenv('CHATTERBOX_CPP_CWD', str(model.parent))).expanduser()
    return runtime_env, cwd


def _validate_chatterbox_cpp_paths() -> tuple[Path, Path, Path, Path]:
    required = {
        'CHATTERBOX_CPP_BINARY': CHATTERBOX_CPP_BINARY,
        'CHATTERBOX_CPP_MODEL': CHATTERBOX_CPP_MODEL,
        'CHATTERBOX_CPP_S3GEN': CHATTERBOX_CPP_S3GEN,
        'CHATTERBOX_CPP_PROFILE': CHATTERBOX_CPP_PROFILE,
    }
    missing = [name for name, value in required.items() if not value]
    if missing:
        raise RuntimeError('Set ' + ', '.join(missing) + ' for TTS_BACKEND=chatterbox_cpp')
    paths = tuple(Path(value).expanduser() for value in required.values())
    for name, path in zip(required, paths):
        if not path.exists():
            raise RuntimeError(f'{name} does not exist: {path}')
    return paths  # type: ignore[return-value]


def build_chatterbox_cpp_command(*, text: Optional[str], output: str | Path,
                                  input_file: Optional[str | Path] = None,
                                  eof_marker: Optional[str] = None) -> list[str]:
    """Build one native command for batch or live PCM synthesis."""
    binary, model, s3gen, profile = _validate_chatterbox_cpp_paths()
    command = [str(binary), '--model', str(model), '--s3gen-gguf', str(s3gen),
               '--ref-dir', str(profile)]
    if input_file is not None:
        if not eof_marker:
            raise ValueError('eof_marker is required with input_file')
        command.extend(['--input-file', str(input_file), '--input-eof-marker',
                        eof_marker, '--input-by-line'])
    elif text is not None:
        command.extend(['--text', text])
    else:
        raise ValueError('text or input_file is required')
    command.extend(['--out', str(output), '--n-gpu-layers', str(CHATTERBOX_CPP_GPU_LAYERS),
                    '--threads', str(CHATTERBOX_CPP_THREADS), '--n-predict',
                    str(CHATTERBOX_CPP_N_PREDICT), '--seed', str(CHATTERBOX_CPP_SEED),
                    '--max-sentence-chars', str(CHATTERBOX_CPP_MAX_SENTENCE_CHARS)])
    if CHATTERBOX_CPP_STREAM_CHUNK_TOKENS > 0:
        command.extend(['--stream-chunk-tokens', str(CHATTERBOX_CPP_STREAM_CHUNK_TOKENS),
                        '--stream-first-chunk-tokens', str(CHATTERBOX_CPP_STREAM_FIRST_CHUNK_TOKENS)])
    if CHATTERBOX_CPP_VERBOSE:
        command.append('--verbose')
    return command


def _write_pcm_wav(path: Path, pcm: bytes, *, sample_rate: int = 16000,
                   channels: int = 2) -> None:
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(pcm)


class NativePcmStreamSession:
    """Run native live-input TTS and expose normalized PCM bytes."""

    def __init__(self, directory: Path, *, on_first_audio=None, on_complete=None):
        self.directory = directory
        self.input_path = directory / 'native-live-input.txt'
        self.raw_path = directory / 'native-stream.pcm'
        self.fallback_path = directory / 'native-stream.wav'
        self.eof_marker = f'__ARLO_EOF_{uuid.uuid4().hex}__'
        self.chunks: queue.Queue[Optional[bytes]] = queue.Queue(maxsize=128)
        self.on_first_audio = on_first_audio
        self.on_complete = on_complete
        self.first_audio_seen = False
        self.error = ''
        self._cancel = threading.Event()
        self._done = threading.Event()
        self._input_lock = threading.Lock()
        self._native = None
        self._ffmpeg = None
        self._input = None
        self._stderr = bytearray()
        self._stderr_lock = threading.Lock()
        self._start()

    def _start(self) -> None:
        binary, model, _, _ = _validate_chatterbox_cpp_paths()
        self.input_path.write_text('', encoding='utf-8')
        command = build_chatterbox_cpp_command(text=None, output='-',
                                                input_file=self.input_path,
                                                eof_marker=self.eof_marker)
        runtime_env, cwd = _chatterbox_cpp_runtime(binary, model)
        ffmpeg_command = ['ffmpeg', '-hide_banner', '-loglevel', 'error',
                          '-f', 's16le', '-ar', '24000', '-ac', '1', '-i', 'pipe:0',
                          '-f', 's16le', '-ar', '16000', '-ac', '2', '-c:a',
                          'pcm_s16le', '-flush_packets', '1', 'pipe:1']
        self._ffmpeg = subprocess.Popen(ffmpeg_command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        bufsize=0)
        self._native = subprocess.Popen(command, cwd=str(cwd), env=runtime_env,
                                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, bufsize=0)
        self._input = self.input_path.open('a', encoding='utf-8', newline='')
        for process in (self._native, self._ffmpeg):
            threading.Thread(target=self._collect_stderr, args=(process,), daemon=True).start()
        threading.Thread(target=self._pump_native, daemon=True).start()
        threading.Thread(target=self._pump_ffmpeg, daemon=True).start()

    def _collect_stderr(self, process) -> None:
        if process.stderr is None:
            return
        while True:
            data = process.stderr.read(4096)
            if not data:
                return
            with self._stderr_lock:
                self._stderr.extend(data)
                if len(self._stderr) > 16000:
                    del self._stderr[:-16000]

    def _pump_native(self) -> None:
        try:
            assert self._native is not None and self._ffmpeg is not None
            assert self._native.stdout is not None and self._ffmpeg.stdin is not None
            while not self._cancel.is_set():
                data = self._native.stdout.read(65536)
                if not data:
                    break
                self._ffmpeg.stdin.write(data)
                self._ffmpeg.stdin.flush()
            self._ffmpeg.stdin.close()
        except (BrokenPipeError, OSError) as exc:
            if not self._cancel.is_set():
                self.error = f'native PCM pipe failed: {exc}'
            try:
                if self._ffmpeg and self._ffmpeg.stdin:
                    self._ffmpeg.stdin.close()
            except OSError:
                pass

    def _pump_ffmpeg(self) -> None:
        try:
            assert self._ffmpeg is not None and self._ffmpeg.stdout is not None
            with self.raw_path.open('wb') as raw:
                while not self._cancel.is_set():
                    data = self._ffmpeg.stdout.read(65536)
                    if not data:
                        break
                    raw.write(data)
                    if not self.first_audio_seen:
                        self.first_audio_seen = True
                        if self.on_first_audio:
                            self.on_first_audio()
                    self.chunks.put(data)
        except (OSError, BrokenPipeError) as exc:
            if not self._cancel.is_set():
                self.error = f'normalized PCM stream failed: {exc}'
        finally:
            self._finish()

    def _finish(self) -> None:
        native_code = self._native.wait(timeout=30) if self._native else 0
        ffmpeg_code = self._ffmpeg.wait(timeout=30) if self._ffmpeg else 0
        if not self._cancel.is_set() and native_code != 0 and not self.error:
            self.error = f'native TTS exited with {native_code}'
        if not self._cancel.is_set() and ffmpeg_code != 0 and not self.error:
            self.error = f'ffmpeg PCM conversion exited with {ffmpeg_code}'
        if not self.error and self.raw_path.exists():
            _write_pcm_wav(self.fallback_path, self.raw_path.read_bytes())
        self.chunks.put(None)
        self._done.set()
        if self.on_complete:
            self.on_complete(self)

    def write_sentence(self, sentence: str) -> None:
        if not sentence.strip():
            return
        with self._input_lock:
            if self._input is None or self._done.is_set():
                raise RuntimeError('native live TTS is no longer accepting text')
            self._input.write(sentence.strip() + '\n')
            self._input.flush()

    def finish_input(self) -> None:
        with self._input_lock:
            if self._input is not None:
                self._input.write(self.eof_marker)
                self._input.flush()
                self._input.close()
                self._input = None

    def iter_chunks(self):
        while True:
            chunk = self.chunks.get()
            if chunk is None:
                return
            yield chunk

    def wait(self, timeout: Optional[float] = None) -> bool:
        return self._done.wait(timeout)

    def cancel(self) -> None:
        self._cancel.set()
        with self._input_lock:
            if self._input is not None:
                self._input.close()
                self._input = None
        for process in (self._native, self._ffmpeg):
            if process is not None and process.poll() is None:
                process.terminate()
        try:
            self.chunks.put_nowait(None)
        except queue.Full:
            pass

jobs: dict[str, Job] = {}
jobs_lock = threading.Lock()
run_lock = threading.Lock()  # keep a single voice turn active at once
stt_lock = threading.Lock()
whisper_model = None
stt_worker_lock = threading.Lock()
stt_worker_process = None
chatterbox_lock = threading.Lock()
chatterbox_model = None
_timing_context = threading.local()
history: list[dict[str, str]] = []


def _add_timing(name: str, milliseconds: float) -> None:
    metrics = getattr(_timing_context, 'metrics', None)
    if metrics is not None:
        metrics[name] = metrics.get(name, 0.0) + milliseconds


def _turn_elapsed_ms(moment: Optional[float] = None) -> float:
    current = time.perf_counter() if moment is None else moment
    started = getattr(_timing_context, 'turn_started', None)
    if started is None:
        return 0.0
    return (current - started) * 1000


def _set_first_timing(name: str, moment: Optional[float] = None) -> None:
    metrics = getattr(_timing_context, 'metrics', None)
    if metrics is not None and name not in metrics:
        metrics[name] = _turn_elapsed_ms(moment)


def _record_hermes_event(event: dict) -> dict:
    """Record provider metadata without retaining streamed content."""
    metrics = getattr(_timing_context, 'metrics', None)
    choices = event.get('choices') or [{}]
    choice = choices[0] if isinstance(choices[0], dict) else {}
    delta = choice.get('delta') or {}
    if not isinstance(delta, dict):
        delta = {}

    if metrics is not None:
        model = event.get('model')
        if model and 'hermes_model' not in metrics:
            metrics['hermes_model'] = str(model)
        usage = event.get('usage') or choice.get('usage')
        if isinstance(usage, dict):
            for source, target in (
                ('prompt_tokens', 'hermes_prompt_tokens'),
                ('completion_tokens', 'hermes_completion_tokens'),
                ('total_tokens', 'hermes_total_tokens'),
            ):
                value = usage.get(source)
                if isinstance(value, (int, float)):
                    metrics[target] = value
        reasoning_chars = 0
        for key in ('reasoning_content', 'reasoning', 'analysis'):
            value = delta.get(key)
            if isinstance(value, str):
                reasoning_chars += len(value)
        if reasoning_chars:
            metrics['hermes_reasoning_chars'] = (
                metrics.get('hermes_reasoning_chars', 0) + reasoning_chars
            )
        tool_calls = delta.get('tool_calls') or choice.get('tool_calls')
        if tool_calls:
            metrics['hermes_tool_call_chunks'] = (
                metrics.get('hermes_tool_call_chunks', 0) + len(tool_calls)
                if isinstance(tool_calls, list) else
                metrics.get('hermes_tool_call_chunks', 0) + 1
            )
            _set_first_timing('hermes_first_tool_call_ms')
    return delta


SYSTEM_PROMPT = (
    "You are ARLO, the user's friendly, concise desktop AI companion. "
    "You are speaking aloud through a small robot. Reply naturally in 1-3 sentences "
    "and keep spoken replies under 60 words unless the user asks for detail. "
    "Do not claim that you saw a room, checked a file, "
    "or used a tool unless you actually did. Do not use markdown or emoji in spoken replies."
)


def get_system_prompt() -> str:
    """Optionally layer a Hermes Bot soul onto the spoken bridge prompt."""
    if not HERMES_SOUL_FILE:
        return SYSTEM_PROMPT
    try:
        soul = Path(HERMES_SOUL_FILE).expanduser().read_text(encoding='utf-8').strip()
    except OSError:
        return SYSTEM_PROMPT
    if not soul:
        return SYSTEM_PROMPT
    return f'{soul}\n\nOperational speaking requirements:\n{SYSTEM_PROMPT}'


def require_key(key: Optional[str]):
    # An absent key is insecure. Fail closed, including on /health.
    if not DEVICE_KEY or DEVICE_KEY.startswith('replace-with-') or key != DEVICE_KEY:
        raise HTTPException(status_code=401, detail='Invalid device key')


def cleanup_jobs():
    now = time.time()
    with jobs_lock:
        expired = [jid for jid, job in jobs.items()
                   if now - job.created > JOB_TTL and job.state in (
                       'ready', 'no_speech', 'conversation_end', 'error', 'cancelled')]
        for jid in expired:
            job = jobs[jid]
            try:
                job.directory.cleanup()
            except OSError:
                # Windows can temporarily lock reply.wav while the K10 is
                # downloading it. Keep the terminal job so cleanup can retry
                # on the next turn instead of taking down the bridge.
                continue
            jobs.pop(jid, None)


def get_job(jid: str) -> Job:
    with jobs_lock:
        job = jobs.get(jid)
    if job is None:
        raise HTTPException(404, 'Unknown job')
    return job


def is_cancelled(job: Job) -> bool:
    with jobs_lock:
        return job.cancel_requested or job.state == 'cancelled'


def cancel_job(job: Job) -> None:
    with jobs_lock:
        job.cancel_requested = True
        job.state = 'cancelled'
    session = job.pcm_stream_session
    if session is not None:
        session.cancel()


def _start_stt_worker():
    global stt_worker_process
    worker = Path(__file__).with_name('stt_worker.py')
    stt_worker_process = subprocess.Popen(
        [STT_WORKER_PYTHON, str(worker), '--serve'],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )
    return stt_worker_process


def _stop_stt_worker() -> None:
    global stt_worker_process
    process = stt_worker_process
    stt_worker_process = None
    if process is not None and process.poll() is None:
        process.terminate()


def speech_to_text(path: Path) -> str:
    global whisper_model
    if STT_WORKER_PYTHON:
        global stt_worker_process
        with stt_worker_lock:
            for attempt in range(2):
                process = stt_worker_process
                if process is None or process.poll() is not None:
                    process = _start_stt_worker()
                try:
                    assert process.stdin is not None
                    assert process.stdout is not None
                    process.stdin.write(json.dumps({'path': str(path)}) + '\n')
                    process.stdin.flush()
                    payload = json.loads(process.stdout.readline())
                    return str(payload.get('transcript', ''))[:1200]
                except (AssertionError, OSError, ValueError, json.JSONDecodeError):
                    _stop_stt_worker()
                    if attempt == 1:
                        raise
        raise RuntimeError('STT worker did not return a transcript')
    with stt_lock:
        if whisper_model is None:
            from faster_whisper import WhisperModel
            whisper_model = WhisperModel(
                STT_MODEL,
                device=os.getenv('STT_DEVICE', 'auto'),
                compute_type=os.getenv('STT_COMPUTE_TYPE', 'default'),
            )
        segments, _ = whisper_model.transcribe(
            str(path), language='en', beam_size=3, vad_filter=True,
            condition_on_previous_text=False,
        )
        transcript = ' '.join(s.text.strip() for s in segments).strip()
    # Common Whisper hallucinations from silent recordings.
    if transcript.lower().strip(' .!?') in {
        '', 'thank you', 'thanks for watching', 'you', 'bye', 'subtitles by the amara.org community'
    }:
        return ''
    return transcript[:1200]


def ask_hermes(text: str, on_sentence=None) -> str:
    if MOCK_HERMES:
        result = f'I heard you say: {text}. My Hermes connection is in test mode.'
        if on_sentence:
            for segment in split_spoken_segments(result):
                on_sentence(segment)
        return result
    if not HERMES_KEY or HERMES_KEY.startswith('replace-with-'):
        raise RuntimeError('Set HERMES_KEY in bridge/.env')
    messages = [{'role': 'system', 'content': get_system_prompt()}, *history[-16:],
                {'role': 'user', 'content': text}]
    request_body = {
        'model': HERMES_MODEL,
        'messages': messages,
        'stream': on_sentence is not None,
        'max_tokens': HERMES_MAX_TOKENS,
    }
    if HERMES_PROVIDER:
        request_body['provider'] = HERMES_PROVIDER
    headers = {
        'Authorization': f'Bearer {HERMES_KEY}',
        'X-Hermes-Session-Id': 'arlo-mini-local',
        'X-Hermes-Session-Key': 'agent:main:arlo-mini:dm:owner',
    }
    request_started = time.perf_counter()
    metrics = getattr(_timing_context, 'metrics', None)
    if metrics is not None:
        _set_first_timing('hermes_request_start_ms', request_started)
        metrics['hermes_input_chars'] = sum(
            len(str(message.get('content', ''))) for message in messages
        )
        metrics['hermes_history_messages'] = max(0, len(messages) - 2)
    response = requests.post(HERMES_URL, headers=headers, json=request_body,
                             timeout=(5, 240), stream=on_sentence is not None)
    response.raise_for_status()
    if on_sentence is None:
        payload = response.json()
        _record_hermes_event(payload)
        result = payload['choices'][0]['message']['content']
    else:
        pieces = []
        pending = ''
        for raw_line in response.iter_lines(decode_unicode=True):
            line = raw_line.strip() if isinstance(raw_line, str) else raw_line.decode().strip()
            if not line:
                continue
            if metrics is not None:
                _set_first_timing('hermes_first_sse_event_ms')
            if line == 'data: [DONE]':
                continue
            if line.startswith('data:'):
                line = line[5:].strip()
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            delta_object = _record_hermes_event(event)
            choice = (event.get('choices') or [{}])[0]
            delta = delta_object.get('content')
            if delta is None:
                delta = choice.get('message', {}).get('content', '')
            if isinstance(delta, list):
                delta = ''.join(p.get('text', '') for p in delta if isinstance(p, dict))
            if not delta:
                continue
            delta = str(delta)
            if metrics is not None:
                _set_first_timing('hermes_first_content_token_ms')
                _set_first_timing('hermes_first_token_ms')
            pieces.append(delta)
            pending += delta
            complete, pending = consume_stream_segments(pending)
            for segment in complete:
                if metrics is not None:
                    _set_first_timing('hermes_first_phrase_ms')
                    metrics.setdefault('hermes_first_phrase_words', _word_count(segment))
                on_sentence(segment)
        complete, pending = consume_stream_segments(pending, final=True)
        for segment in complete:
            if metrics is not None:
                _set_first_timing('hermes_first_phrase_ms')
                metrics.setdefault('hermes_first_phrase_words', _word_count(segment))
            on_sentence(segment)
        result = ''.join(pieces)
    # Responses may contain non-text blocks for some providers.
    if isinstance(result, list):
        result = ' '.join(p.get('text', '') for p in result if isinstance(p, dict))
    result = str(result).strip()
    if not result:
        result = "I'm here, but my response came back empty."
    if metrics is not None:
        metrics['hermes_output_chars'] = len(result)
        metrics['hermes_output_words'] = _word_count(result)
        completion_tokens = metrics.get('hermes_completion_tokens')
        elapsed = time.perf_counter() - request_started
        if completion_tokens and elapsed > 0:
            metrics['hermes_stream_tokens_per_s'] = completion_tokens / elapsed
    history.extend([{'role': 'user', 'content': text},
                    {'role': 'assistant', 'content': result}])
    if len(history) > 20:
        del history[:-20]
    return result


def limit_chatterbox_generation(model) -> None:
    """Bound Turbo speech-token generation so one turn cannot run too long."""
    if getattr(model, '_arlo_generation_limited', False):
        return
    original = model.t3.inference_turbo

    def limited(*args, **kwargs):
        kwargs['max_gen_len'] = CHATTERBOX_MAX_GEN_LEN
        return original(*args, **kwargs)

    model.t3.inference_turbo = limited
    model._arlo_generation_limited = True


def _load_chatterbox_model():
    global chatterbox_model
    with chatterbox_lock:
        if chatterbox_model is None:
            import torch
            from chatterbox.tts_turbo import ChatterboxTurboTTS

            device = CHATTERBOX_DEVICE
            if device == 'auto':
                device = 'cuda' if torch.cuda.is_available() else 'cpu'
            try:
                chatterbox_model = ChatterboxTurboTTS.from_pretrained(
                    device=device,
                    nano=CHATTERBOX_NANO,
                )
            except TypeError as exc:
                if CHATTERBOX_NANO and 'nano' in str(exc).lower():
                    raise RuntimeError(
                        'CHATTERBOX_NANO=true requires a Chatterbox build with '
                        'Nano support; update chatterbox-tts from the official '
                        'repository'
                    ) from exc
                raise
            limit_chatterbox_generation(chatterbox_model)
        return chatterbox_model


def _prepare_chatterbox_reference(model) -> None:
    if not CHATTERBOX_REFERENCE:
        raise RuntimeError('Set CHATTERBOX_REFERENCE to a clean reference WAV')
    reference = Path(CHATTERBOX_REFERENCE).expanduser()
    if not reference.is_file():
        raise RuntimeError(f'CHATTERBOX_REFERENCE does not exist: {reference}')
    with chatterbox_lock:
        reference_key = str(reference)
        if getattr(model, '_arlo_reference', None) != reference_key:
            model.prepare_conditionals(reference_key)
            model._arlo_reference = reference_key


def preload_chatterbox() -> None:
    """Load Turbo and its reference conditionals before the first turn."""
    model = _load_chatterbox_model()
    _prepare_chatterbox_reference(model)


def run_chatterbox_tts(text: str, raw: Path, directory: Path) -> None:
    """Generate a cloned voice clip with the warmed Turbo model."""
    del directory  # The reference is intentionally stable across jobs.
    if not CHATTERBOX_REFERENCE:
        raise RuntimeError('Set CHATTERBOX_REFERENCE to a clean reference WAV')
    reference = Path(CHATTERBOX_REFERENCE).expanduser()
    if not reference.is_file():
        raise RuntimeError(f'CHATTERBOX_REFERENCE does not exist: {reference}')
    model = _load_chatterbox_model()
    _prepare_chatterbox_reference(model)

    import torch
    import torchaudio

    generation_started = time.perf_counter()
    wav = model.generate(text)
    _add_timing('tts_generation_ms', (time.perf_counter() - generation_started) * 1000)
    if not isinstance(wav, torch.Tensor):
        wav = torch.as_tensor(wav)
    if wav.ndim == 1:
        wav = wav.unsqueeze(0)
    torchaudio.save(str(raw), wav.detach().cpu(), model.sr)


def _run_tts(text: str, raw: Path, directory: Path) -> None:
    backend = TTS_BACKEND
    if backend == 'auto':
        backend = 'sapi' if platform.system() == 'Windows' else 'espeak'
    if backend == 'piper':
        model = os.getenv('PIPER_MODEL', '')
        if not model:
            raise RuntimeError('PIPER_MODEL must point to a Piper .onnx voice')
        subprocess.run([os.getenv('PIPER_BINARY', 'piper'), '--model', model,
                        '--output_file', str(raw)], input=text, text=True,
                       capture_output=True, timeout=90, check=True)
    elif backend == 'espeak':
        subprocess.run(['espeak-ng', '-w', str(raw), text], capture_output=True,
                       text=True, check=True, timeout=60)
    elif backend == 'sapi':
        # Pass text via a file to avoid PowerShell quoting/injection problems.
        text_file = directory / 'tts.txt'
        text_file.write_text(text, encoding='utf-8')
        # Use a temporary .ps1 file so Windows PowerShell receives the two
        # paths as $args instead of parsing them as part of -Command.
        script_file = directory / 'tts.ps1'
        script_file.write_text(
            'Add-Type -AssemblyName System.Speech; '
            '$s=[IO.File]::ReadAllText($args[0],[Text.Encoding]::UTF8); '
            '$v=New-Object System.Speech.Synthesis.SpeechSynthesizer; '
            '$v.SetOutputToWaveFile($args[1]); '
            '$v.Speak($s); $v.Dispose()',
            encoding='utf-8',
        )
        subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive',
                        '-ExecutionPolicy', 'Bypass',
                        '-File', str(script_file), str(text_file), str(raw)],
                       capture_output=True, text=True, check=True, timeout=90)
    elif backend == 'chatterbox':
        run_chatterbox_tts(text, raw, directory)
    elif backend == 'chatterbox_cpp':
        run_chatterbox_cpp_tts(text, raw, directory)
    else:
        raise RuntimeError(f'Unknown TTS_BACKEND {backend}')


def run_chatterbox_cpp_tts(text: str, raw: Path, directory: Path) -> None:
    """Generate a cloned voice clip with the isolated CUDA C++ runtime."""
    del directory
    required = {
        'CHATTERBOX_CPP_BINARY': CHATTERBOX_CPP_BINARY,
        'CHATTERBOX_CPP_MODEL': CHATTERBOX_CPP_MODEL,
        'CHATTERBOX_CPP_S3GEN': CHATTERBOX_CPP_S3GEN,
        'CHATTERBOX_CPP_PROFILE': CHATTERBOX_CPP_PROFILE,
    }
    missing = [name for name, value in required.items() if not value]
    if missing:
        raise RuntimeError(
            'Set ' + ', '.join(missing) + ' for TTS_BACKEND=chatterbox_cpp'
        )

    binary = Path(CHATTERBOX_CPP_BINARY).expanduser()
    model = Path(CHATTERBOX_CPP_MODEL).expanduser()
    s3gen = Path(CHATTERBOX_CPP_S3GEN).expanduser()
    profile = Path(CHATTERBOX_CPP_PROFILE).expanduser()
    for name, path in (
        ('CHATTERBOX_CPP_BINARY', binary),
        ('CHATTERBOX_CPP_MODEL', model),
        ('CHATTERBOX_CPP_S3GEN', s3gen),
        ('CHATTERBOX_CPP_PROFILE', profile),
    ):
        if not path.exists():
            raise RuntimeError(f'{name} does not exist: {path}')

    command = [
        str(binary), '--model', str(model), '--s3gen-gguf', str(s3gen),
        '--ref-dir', str(profile), '--text', text, '--out', str(raw),
        '--n-gpu-layers', str(CHATTERBOX_CPP_GPU_LAYERS),
        '--threads', str(CHATTERBOX_CPP_THREADS),
        '--n-predict', str(CHATTERBOX_CPP_N_PREDICT),
        '--seed', str(CHATTERBOX_CPP_SEED),
        '--max-sentence-chars', str(CHATTERBOX_CPP_MAX_SENTENCE_CHARS),
    ]
    if CHATTERBOX_CPP_STREAM_CHUNK_TOKENS > 0:
        command.extend([
            '--stream-chunk-tokens', str(CHATTERBOX_CPP_STREAM_CHUNK_TOKENS),
        ])
        if CHATTERBOX_CPP_STREAM_FIRST_CHUNK_TOKENS > 0:
            command.extend([
                '--stream-first-chunk-tokens',
                str(CHATTERBOX_CPP_STREAM_FIRST_CHUNK_TOKENS),
            ])
    if CHATTERBOX_CPP_VERBOSE:
        command.append('--verbose')

    runtime_env = os.environ.copy()
    runtime_paths = [str(binary.parent), str(model.parent)]
    if CHATTERBOX_CPP_RUNTIME_PATHS:
        runtime_paths.extend(
            part for part in CHATTERBOX_CPP_RUNTIME_PATHS.split(os.pathsep) if part
        )
    runtime_env['PATH'] = os.pathsep.join(runtime_paths + [runtime_env.get('PATH', '')])
    cwd = Path(os.getenv('CHATTERBOX_CPP_CWD', str(model.parent))).expanduser()

    started = time.perf_counter()
    try:
        result = subprocess.run(
            command, cwd=str(cwd), env=runtime_env, capture_output=True,
            text=True, encoding='utf-8', errors='replace', check=True,
            timeout=CHATTERBOX_CPP_TIMEOUT,
        )
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or '').strip()[-1200:]
        raise RuntimeError(
            f'Chatterbox.cpp exited with {exc.returncode}: {detail}'
        ) from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError('Chatterbox.cpp timed out') from exc

    _add_timing('tts_generation_ms', (time.perf_counter() - started) * 1000)
    output = f'{result.stdout}\n{result.stderr}'
    for metric, pattern in {
        'chatterbox_cpp_load_ms': r'BENCH: T3_LOAD_MS=(\d+(?:\.\d+)?)',
        'chatterbox_cpp_t3_ms': r'BENCH: T3_INFER_MS=(\d+(?:\.\d+)?)',
        'chatterbox_cpp_s3gen_ms': r'BENCH: S3GEN_INFER_MS=(\d+(?:\.\d+)?)',
        'chatterbox_cpp_audio_ms': r'BENCH: S3GEN_INFER_MS=\d+(?:\.\d+)? AUDIO_MS=(\d+(?:\.\d+)?)',
    }.items():
        match = re.search(pattern, output)
        if match:
            _add_timing(metric, float(match.group(1)))

    if not raw.exists() or raw.stat().st_size < 44:
        raise RuntimeError('Chatterbox.cpp produced no audio')


def speech_to_wav(text: str, out_path: Path, directory: Path) -> None:
    raw_path = directory / 'speech-raw.wav'
    # Keep the complete Hermes answer. The K10 now waits for the playback task
    # to finish, so long responses are no longer silently cut at 700 characters.
    _run_tts(text, raw_path, directory)
    if not raw_path.exists() or raw_path.stat().st_size < 44:
        raise RuntimeError('TTS produced no audio')
    # K10 examples document stereo WAV playback. Normalize the format so the
    # hardware decoder receives one consistent PCM layout.
    normalize_started = time.perf_counter()
    subprocess.run(
        ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i', str(raw_path),
         '-ac', '2', '-ar', '16000', '-acodec', 'pcm_s16le', str(out_path)],
        capture_output=True, text=True, check=True, timeout=90,
    )
    _add_timing('normalize_ms', (time.perf_counter() - normalize_started) * 1000)
    with wave.open(str(out_path), 'rb') as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2 or w.getframerate() != 16000:
            raise RuntimeError('Unexpected speech WAV format')


def run_job(jid: str, input_path: Path) -> None:
    job = get_job(jid)
    started = time.perf_counter()
    timings: dict[str, float] = {}
    job.timings = timings
    _timing_context.metrics = timings
    _timing_context.turn_started = started
    try:
        with run_lock:
            if is_cancelled(job):
                return
            job.state = 'transcribing'
            stt_started = time.perf_counter()
            text = speech_to_text(input_path)
            stt_ms = (time.perf_counter() - stt_started) * 1000
            timings['stt_ms'] = stt_ms
            job.transcript = text
            if is_cancelled(job):
                return
            if not text:
                job.state = 'no_speech'
                print(f'[ARLO BRIDGE] job={jid} no_speech stt_ms={stt_ms:.0f}', flush=True)
                return
            end_conversation = is_conversation_end_phrase(text)
            if end_conversation:
                job.answer = 'Goodbye. Talk to you later!'
                hermes_ms = 0.0
            else:
                job.state = 'thinking'
                hermes_started = time.perf_counter()
            if is_cancelled(job):
                return
            job.stream_complete = False
            live_session = None
            if TTS_BACKEND == 'chatterbox_cpp' and CHATTERBOX_CPP_LIVE_STREAM:
                tts_stream_started = time.perf_counter()

                def mark_first_audio() -> None:
                    job.pcm_stream_ready = True
                    job.state = 'streaming'
                    timings.setdefault(
                        'first_audio_ready_ms',
                        (time.perf_counter() - started) * 1000,
                    )

                def mark_stream_complete(session) -> None:
                    job.pcm_stream_complete = True
                    if session.error:
                        job.pcm_stream_error = session.error
                        return
                    if session.fallback_path.exists():
                        job.audio_path = session.fallback_path
                        job.audio_segments = [session.fallback_path]
                        job.segments_ready = 1
                        job.segments_total = 1
                    timings['tts_generation_ms'] = (
                        time.perf_counter() - tts_stream_started
                    ) * 1000

                live_session = NativePcmStreamSession(
                    Path(job.directory.name),
                    on_first_audio=mark_first_audio,
                    on_complete=mark_stream_complete,
                )
                job.pcm_stream_session = live_session
                job.pcm_stream_ready = True

            def render_sentence(sentence: str) -> None:
                if is_cancelled(job):
                    return
                index = len(job.audio_segments)
                target = Path(job.directory.name) / f'reply-{index}.wav'
                if index == 0:
                    _set_first_timing('first_phrase_tts_start_ms')
                    if 'hermes_first_phrase_ms' in timings:
                        timings.setdefault(
                            'first_phrase_to_tts_ms',
                            _turn_elapsed_ms() - timings['hermes_first_phrase_ms'],
                        )
                render_started = time.perf_counter()
                speech_to_wav(sentence, target, Path(job.directory.name))
                _add_timing('segment_render_ms',
                            (time.perf_counter() - render_started) * 1000)
                job.audio_segments.append(target)
                job.segments_ready = index + 1
                if index == 0:
                    timings['first_audio_ready_ms'] = (
                        time.perf_counter() - started
                    ) * 1000
                if job.audio_path is None:
                    job.audio_path = target
                job.state = 'streaming'

            # With a real Hermes gateway this callback is fed by SSE sentence
            # boundaries, allowing TTS and K10 playback to start before the
            # gateway has finished generating the complete answer.
            if live_session is not None:
                if end_conversation:
                    live_session.write_sentence(job.answer)
                    live_session.finish_input()
                    hermes_ms = 0.0
                else:
                    job.state = 'thinking'
                    job.answer = ask_hermes(text, on_sentence=live_session.write_sentence)
                    hermes_ms = (time.perf_counter() - hermes_started) * 1000
                    live_session.finish_input()
                if not live_session.wait(CHATTERBOX_CPP_TIMEOUT + 30):
                    live_session.cancel()
                    raise RuntimeError('native live TTS timed out')
                if live_session.error:
                    detail = bytes(live_session._stderr).decode('utf-8', errors='replace')[-800:]
                    raise RuntimeError(f'{live_session.error}: {detail}'.strip())
            elif end_conversation:
                render_sentence(job.answer)
            else:
                job.state = 'thinking'
                if TTS_BACKEND == 'chatterbox_cpp' and CHATTERBOX_CPP_BATCH:
                    # The native CLI can auto-split a complete answer inside one
                    # warm model invocation. This avoids reloading GGUF weights for
                    # every sentence while keeping the Python backend's streaming
                    # behavior unchanged.
                    job.answer = ask_hermes(text)
                    hermes_ms = (time.perf_counter() - hermes_started) * 1000
                    if job.answer.strip():
                        render_sentence(job.answer)
                else:
                    job.answer = ask_hermes(text, on_sentence=render_sentence)
                    hermes_ms = (time.perf_counter() - hermes_started) * 1000
                    if not job.audio_segments:
                        for segment in split_spoken_segments(job.answer):
                            render_sentence(segment)
            timings['hermes_ms'] = hermes_ms
            job.segments_total = len(job.audio_segments)
            timings['tts_ms'] = (
                timings.get('tts_generation_ms', 0.0)
                + timings.get('normalize_ms', 0.0)
            )
            if is_cancelled(job):
                return
            job.stream_complete = True
            job.state = 'conversation_end' if end_conversation else 'ready'
            total_ms = (time.perf_counter() - started) * 1000
            timings['total_ms'] = total_ms
            print(
                f'[ARLO BRIDGE] job={jid} ready stt_ms={stt_ms:.0f} '
                f'hermes_ms={hermes_ms:.0f} tts_ms={timings["tts_ms"]:.0f} '
                f'first_audio_ms={timings.get("first_audio_ready_ms", 0):.0f} '
                f'total_ms={total_ms:.0f}',
                flush=True,
            )
    except Exception as exc:
        if is_cancelled(job):
            return
        # Error detail deliberately exposed only to a client possessing the device key.
        job.error = f'{type(exc).__name__}: {str(exc)[:240]}'
        job.state = 'error'
        total_ms = (time.perf_counter() - started) * 1000
        timings['total_ms'] = total_ms
        print(f'[ARLO BRIDGE] job={jid} error total_ms={total_ms:.0f}', flush=True)
    finally:
        if getattr(_timing_context, 'metrics', None) is timings:
            del _timing_context.metrics
        if hasattr(_timing_context, 'turn_started'):
            del _timing_context.turn_started


@app.get('/health')
def health(x_device_key: Optional[str] = Header(default=None)):
    require_key(x_device_key)
    return {'ok': True, 'name': 'ARLO Mini bridge', 'mock_hermes': MOCK_HERMES}


@app.post('/turn', status_code=202)
async def start_turn(request: Request, background_tasks: BackgroundTasks,
                     x_device_key: Optional[str] = Header(default=None)):
    require_key(x_device_key)
    cleanup_jobs()
    with jobs_lock:
        busy = any(j.state not in (
            'ready', 'no_speech', 'conversation_end', 'error', 'cancelled')
                   for j in jobs.values())
    if busy:
        raise HTTPException(409, 'Bridge is busy')
    # Stream read with strict byte limit; reject oversized uploads before creating a job.
    chunks = []
    total = 0
    async for chunk in request.stream():
        total += len(chunk)
        if total > MAX_UPLOAD:
            raise HTTPException(413, 'Audio exceeds 2 MB limit')
        chunks.append(chunk)
    data = b''.join(chunks)
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise HTTPException(400, 'Expected RIFF/WAVE data')
    jid = uuid.uuid4().hex
    td = tempfile.TemporaryDirectory(prefix='arlo-mini-')
    path = Path(td.name) / 'input.wav'
    path.write_bytes(data)
    with jobs_lock:
        jobs[jid] = Job(id=jid, directory=td)
    background_tasks.add_task(run_job, jid, path)
    return {'id': jid, 'state': 'queued'}


@app.get('/jobs/{jid}')
def job_status(jid: str, x_device_key: Optional[str] = Header(default=None)):
    require_key(x_device_key)
    job = get_job(jid)
    return {'id': job.id, 'state': job.state, 'transcript': job.transcript,
            'answer': job.answer if job.state in ('ready', 'conversation_end') else '',
            'error': job.error, 'segments_ready': job.segments_ready,
            'segments_total': job.segments_total,
            'stream_complete': job.stream_complete,
            'pcm_stream_ready': job.pcm_stream_ready,
            'pcm_stream_complete': job.pcm_stream_complete,
            'pcm_stream_error': job.pcm_stream_error,
            'pcm_stream_claimed': job.pcm_stream_claimed,
            'audio_format': {
                'encoding': 's16le', 'sample_rate': 16000,
                'channels': 2, 'bits_per_sample': 16,
            } if job.pcm_stream_ready else None,
            'timings': {
                key: round(value, 1) if isinstance(value, (int, float)) else value
                for key, value in job.timings.items()
            }}


@app.post('/jobs/{jid}/cancel')
def cancel_job_endpoint(jid: str, x_device_key: Optional[str] = Header(default=None)):
    require_key(x_device_key)
    job = get_job(jid)
    cancel_job(job)
    return {'id': job.id, 'state': 'cancelled'}


@app.get('/jobs/{jid}/audio')
def job_audio(jid: str, segment: int = 0,
              x_device_key: Optional[str] = Header(default=None)):
    require_key(x_device_key)
    job = get_job(jid)
    if segment < 0 or segment >= len(job.audio_segments):
        raise HTTPException(409, 'Audio not ready')
    return FileResponse(job.audio_segments[segment], media_type='audio/wav',
                        filename=f'reply-{segment}.wav')


@app.get('/jobs/{jid}/pcm')
def job_pcm(jid: str, x_device_key: Optional[str] = Header(default=None)):
    """Stream normalized S16LE PCM while native synthesis is still running."""
    require_key(x_device_key)
    job = get_job(jid)
    session = job.pcm_stream_session
    if not job.pcm_stream_ready or session is None:
        raise HTTPException(409, 'PCM audio is not ready')
    with job.pcm_stream_claim_lock:
        if job.pcm_stream_claimed:
            raise HTTPException(409, 'PCM stream already claimed')
        job.pcm_stream_claimed = True
    return StreamingResponse(
        session.iter_chunks(),
        media_type='application/octet-stream',
        headers={
            'Cache-Control': 'no-store',
            'X-Arlo-Audio-Encoding': 's16le',
            'X-Arlo-Audio-Sample-Rate': '16000',
            'X-Arlo-Audio-Channels': '2',
            'X-Arlo-Audio-Bits': '16',
        },
    )


if __name__ == '__main__':
    import uvicorn
    uvicorn.run(app, host=os.getenv('BRIDGE_HOST', '0.0.0.0'),
                port=int(os.getenv('BRIDGE_PORT', '8765')), access_log=False)
