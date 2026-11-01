import os
import tempfile
from pathlib import Path

os.environ['DEVICE_KEY'] = 'test-device-secret-not-for-production'
os.environ['MOCK_HERMES'] = 'true'

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bridge'))
import server


def test_live_native_turn_does_not_mark_pcm_ready_before_first_audio(monkeypatch):
    directory = tempfile.TemporaryDirectory(prefix='arlo-test-live-not-ready-')
    jid = 'live-not-ready-test'
    server.jobs[jid] = server.Job(id=jid, directory=directory)

    class FakeSession:
        def __init__(self, path, on_first_audio=None, on_complete=None):
            self.fallback_path = Path(path) / 'native-stream.wav'
            self.fallback_path.write_bytes(b'RIFF-fallback')
            self.error = ''
            self.on_complete = on_complete
            self.on_complete(self)

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


def test_live_native_turn_keeps_complete_wav_fallback(monkeypatch):
    directory = tempfile.TemporaryDirectory(prefix='arlo-test-live-turn-')
    jid = 'live-turn-test'
    server.jobs[jid] = server.Job(id=jid, directory=directory)
    written = []

    class FakeSession:
        def __init__(self, path, on_first_audio=None, on_complete=None):
            self.fallback_path = Path(path) / 'native-stream.wav'
            self.fallback_path.write_bytes(b'RIFF-fallback')
            self.error = ''
            self.on_complete = on_complete
            on_first_audio()
            on_complete(self)

        def write_sentence(self, sentence):
            written.append(sentence)

        def finish_input(self):
            pass

        def wait(self, timeout=None):
            return True

        def cancel(self):
            pass

    monkeypatch.setattr(server, 'TTS_BACKEND', 'chatterbox_cpp')
    monkeypatch.setattr(server, 'CHATTERBOX_CPP_LIVE_STREAM', True)
    monkeypatch.setattr(server, 'speech_to_text', lambda path: 'Give me a live answer.')

    def fake_hermes(text, on_sentence=None):
        answer = 'Here is the live answer.'
        if on_sentence:
            on_sentence(answer)
        return answer

    monkeypatch.setattr(server, 'ask_hermes', fake_hermes)
    monkeypatch.setattr(server, 'NativePcmStreamSession', FakeSession)
    try:
        server.run_job(jid, Path(directory.name) / 'input.wav')
        job = server.jobs[jid]
        assert job.state == 'ready'
        assert job.pcm_stream_initialized is True
        assert job.pcm_stream_ready is True
        assert job.pcm_stream_complete is True
        assert job.audio_segments == [Path(directory.name) / 'native-stream.wav']
        assert written == ['Here is the live answer.']
    finally:
        server.jobs.pop(jid, None)
        directory.cleanup()
