import os
import tempfile
from pathlib import Path

os.environ['DEVICE_KEY'] = 'test-device-secret-not-for-production'
os.environ['MOCK_HERMES'] = 'true'

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bridge'))
import server
from fastapi.testclient import TestClient


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_SOURCE = (ROOT / 'firmware' / 'src' / 'main.cpp').read_text()
KEY = {'X-Device-Key': 'test-device-secret-not-for-production'}
client = TestClient(server.app)


def test_live_native_turn_does_not_mark_pcm_ready_before_first_audio(monkeypatch):
    directory = tempfile.TemporaryDirectory(prefix='arlo-test-live-not-ready-')
    jid = 'live-not-ready-test'
    server.jobs[jid] = server.Job(id=jid, directory=directory)

    class FakeSession:
        def __init__(self, path, on_first_audio=None, on_complete=None):
            self.fallback_path = Path(path) / 'native-stream.wav'
            self.fallback_path.write_bytes(b'RIFF-fallback')
            self.error = ''
            on_complete(self)

        def write_sentence(self, sentence):
            pass

        def finish_input(self):
            pass

        def wait(self, timeout=None):
            return True

        def cancel(self):
            pass

    monkeypatch.setattr(server, 'TTS_BACKEND', 'chatterbox_cpp')
    monkeypatch.setattr(server, 'CHATTERBOX_CPP_LIVE_STREAM', True)
    monkeypatch.setattr(server, 'speech_to_text', lambda path: 'Give me a live answer.')
    monkeypatch.setattr(server, 'ask_hermes', lambda text, on_sentence=None: 'Here is the live answer.')
    monkeypatch.setattr(server, 'NativePcmStreamSession', FakeSession)
    try:
        server.run_job(jid, Path(directory.name) / 'input.wav')
        job = server.jobs[jid]
        assert job.pcm_stream_initialized is True
        assert job.pcm_stream_ready is False
        assert job.pcm_stream_complete is True
    finally:
        server.jobs.pop(jid, None)
        directory.cleanup()


def test_job_status_distinguishes_pcm_initialization_from_first_audio():
    directory = tempfile.TemporaryDirectory(prefix='arlo-test-pcm-state-')
    jid = 'pcm-state-test'
    server.jobs[jid] = server.Job(
        id=jid,
        directory=directory,
        state='thinking',
        pcm_stream_initialized=True,
        pcm_stream_ready=False,
    )
    try:
        response = client.get(f'/jobs/{jid}', headers=KEY)
        assert response.status_code == 200
        payload = response.json()
        assert payload['state'] == 'thinking'
        assert payload['pcm_stream_initialized'] is True
        assert payload['pcm_stream_ready'] is False
        assert payload['audio_format'] is not None
    finally:
        server.jobs.pop(jid, None)
        directory.cleanup()


def test_firmware_has_distinct_pcm_lifecycle_states():
    assert 'bool jobPcmStreamInitialized = false;' in FIRMWARE_SOURCE
    assert 'bool jobPcmStreamClaimed = false;' in FIRMWARE_SOURCE
    assert '"pcm_stream_initialized"' in FIRMWARE_SOURCE
    assert 'face("thinking", "Preparing audio...", false, 0);' in FIRMWARE_SOURCE
    assert "!jobPcmStreamClaimed" in FIRMWARE_SOURCE
    assert "jobPcmStreamReady && wantedSegment == 0" in FIRMWARE_SOURCE
