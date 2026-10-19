"""ARLO Mini voice bridge: K10 WAV -> local STT -> Hermes -> local TTS -> K10 WAV.

This is a single-user trusted-LAN prototype. Do not expose port 8765 to the public Internet.
"""

from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import tempfile
import threading
import time
import uuid
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import requests
from dotenv import load_dotenv
from fastapi import BackgroundTasks, FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse

load_dotenv(Path(__file__).with_name('.env'))
app = FastAPI(title="ARLO Mini bridge", docs_url=None, redoc_url=None)

DEVICE_KEY = os.getenv('DEVICE_KEY', '')
HERMES_URL = os.getenv('HERMES_URL', 'http://127.0.0.1:8642/v1/chat/completions')
HERMES_KEY = os.getenv('HERMES_KEY', '')
HERMES_MODEL = os.getenv('HERMES_MODEL', 'hermes-agent')
HERMES_PROVIDER = os.getenv('HERMES_PROVIDER', '')
STT_MODEL = os.getenv('STT_MODEL', 'base.en')
TTS_BACKEND = os.getenv('TTS_BACKEND', 'auto').lower()
MOCK_HERMES = os.getenv('MOCK_HERMES', 'false').lower() == 'true'
MAX_UPLOAD = 2_000_000  # enough for a few seconds of PCM WAV
JOB_TTL = 20 * 60

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
    created: float = field(default_factory=time.time)

jobs: dict[str, Job] = {}
jobs_lock = threading.Lock()
run_lock = threading.Lock()  # keep a single voice turn active at once
stt_lock = threading.Lock()
whisper_model = None
history: list[dict[str, str]] = []

SYSTEM_PROMPT = (
    "You are ARLO, the user's friendly, concise desktop AI companion. "
    "You are speaking aloud through a small robot. Reply naturally in 1-3 sentences "
    "unless the user requests detail. Do not claim that you saw a room, checked a file, "
    "or used a tool unless you actually did. Do not use markdown or emoji in spoken replies."
)


def require_key(key: Optional[str]):
    # An absent key is insecure. Fail closed, including on /health.
    if not DEVICE_KEY or DEVICE_KEY.startswith('replace-with-') or key != DEVICE_KEY:
        raise HTTPException(status_code=401, detail='Invalid device key')


def cleanup_jobs():
    now = time.time()
    with jobs_lock:
        expired = [jid for jid, job in jobs.items()
                   if now - job.created > JOB_TTL and job.state in ('ready', 'no_speech', 'error')]
        for jid in expired:
            jobs.pop(jid).directory.cleanup()


def get_job(jid: str) -> Job:
    with jobs_lock:
        job = jobs.get(jid)
    if job is None:
        raise HTTPException(404, 'Unknown job')
    return job


def speech_to_text(path: Path) -> str:
    global whisper_model
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


def ask_hermes(text: str) -> str:
    if MOCK_HERMES:
        return f'I heard you say: {text}. My Hermes connection is in test mode.'
    if not HERMES_KEY or HERMES_KEY.startswith('replace-with-'):
        raise RuntimeError('Set HERMES_KEY in bridge/.env')
    messages = [{'role': 'system', 'content': SYSTEM_PROMPT}, *history[-16:],
                {'role': 'user', 'content': text}]
    request_body = {
        'model': HERMES_MODEL,
        'messages': messages,
        'stream': False,
    }
    if HERMES_PROVIDER:
        request_body['provider'] = HERMES_PROVIDER
    response = requests.post(
        HERMES_URL,
        headers={
            'Authorization': f'Bearer {HERMES_KEY}',
            'X-Hermes-Session-Id': 'arlo-mini-local',
            'X-Hermes-Session-Key': 'agent:main:arlo-mini:dm:owner',
        },
        json=request_body,
        timeout=(5, 240),
    )
    response.raise_for_status()
    result = response.json()['choices'][0]['message']['content']
    # Responses may contain non-text blocks for some providers.
    if isinstance(result, list):
        result = ' '.join(p.get('text', '') for p in result if isinstance(p, dict))
    result = str(result).strip()
    if not result:
        result = "I'm here, but my response came back empty."
    history.extend([{'role': 'user', 'content': text},
                    {'role': 'assistant', 'content': result}])
    if len(history) > 20:
        del history[:-20]
    return result


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
    else:
        raise RuntimeError(f'Unknown TTS_BACKEND {backend}')


def speech_to_wav(text: str, out_path: Path, directory: Path) -> None:
    raw_path = directory / 'speech-raw.wav'
    # Keep the complete Hermes answer. The K10 now waits for the playback task
    # to finish, so long responses are no longer silently cut at 700 characters.
    _run_tts(text, raw_path, directory)
    if not raw_path.exists() or raw_path.stat().st_size < 44:
        raise RuntimeError('TTS produced no audio')
    # K10 examples document stereo WAV playback. Normalize the format so the
    # hardware decoder receives one consistent PCM layout.
    subprocess.run(
        ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i', str(raw_path),
         '-ac', '2', '-ar', '16000', '-acodec', 'pcm_s16le', str(out_path)],
        capture_output=True, text=True, check=True, timeout=90,
    )
    with wave.open(str(out_path), 'rb') as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2 or w.getframerate() != 16000:
            raise RuntimeError('Unexpected speech WAV format')


def run_job(jid: str, input_path: Path) -> None:
    job = get_job(jid)
    try:
        with run_lock:
            job.state = 'transcribing'
            text = speech_to_text(input_path)
            job.transcript = text
            if not text:
                job.state = 'no_speech'
                return
            job.state = 'thinking'
            answer = ask_hermes(text)
            job.answer = answer
            job.state = 'speaking'
            target = Path(job.directory.name) / 'reply.wav'
            speech_to_wav(answer, target, Path(job.directory.name))
            job.audio_path = target
            job.state = 'ready'
    except Exception as exc:
        # Error detail deliberately exposed only to a client possessing the device key.
        job.error = f'{type(exc).__name__}: {str(exc)[:240]}'
        job.state = 'error'


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
        busy = any(j.state not in ('ready', 'no_speech', 'error') for j in jobs.values())
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
            'answer': job.answer if job.state == 'ready' else '',
            'error': job.error}


@app.get('/jobs/{jid}/audio')
def job_audio(jid: str, x_device_key: Optional[str] = Header(default=None)):
    require_key(x_device_key)
    job = get_job(jid)
    if job.state != 'ready' or job.audio_path is None:
        raise HTTPException(409, 'Audio not ready')
    return FileResponse(job.audio_path, media_type='audio/wav', filename='reply.wav')


if __name__ == '__main__':
    import uvicorn
    uvicorn.run(app, host=os.getenv('BRIDGE_HOST', '0.0.0.0'),
                port=int(os.getenv('BRIDGE_PORT', '8765')), access_log=False)
