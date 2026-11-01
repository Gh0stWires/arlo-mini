"""Offline protocol tests: no Hermes instance, Whisper model, or TTS needed."""

import importlib
import io
import os
import sys
import tempfile
import time
import wave
from pathlib import Path

os.environ['DEVICE_KEY'] = 'test-device-secret-not-for-production'
os.environ['MOCK_HERMES'] = 'true'
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bridge'))
import server
from fastapi.testclient import TestClient

client = TestClient(server.app)
KEY = {'X-Device-Key': 'test-device-secret-not-for-production'}


def make_wav():
    out = io.BytesIO()
    with wave.open(out, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b'\0\0' * 1600)
    return out.getvalue()


def test_health_requires_auth():
    assert client.get('/health').status_code == 401
    assert client.get('/health', headers=KEY).json()['ok'] is True


def test_rejects_invalid_audio():
    assert client.post('/turn', headers=KEY, content=b'not-a-wav').status_code == 400
    assert client.post('/turn', headers=KEY, content=b'Z' * 2_100_000).status_code == 413


def test_silence_returns_no_speech(monkeypatch):
    monkeypatch.setattr(server, 'speech_to_text', lambda p: '')
    response = client.post('/turn', headers=KEY, content=make_wav())
    assert response.status_code == 202
    jid = response.json()['id']
    status = client.get(f'/jobs/{jid}', headers=KEY).json()
    assert status['state'] == 'no_speech'


def test_cleanup_keeps_locked_job_for_a_later_retry(monkeypatch):
    class LockedDirectory:
        def cleanup(self):
            raise OSError('reply audio is still in use')

    job = type('JobStub', (), {
        'created': time.time() - server.JOB_TTL - 1,
        'state': 'conversation_end',
        'directory': LockedDirectory(),
    })()
    jobs = {'locked': job}
    monkeypatch.setattr(server, 'jobs', jobs)

    server.cleanup_jobs()

    assert jobs['locked'] is job


def test_talk_to_you_later_ends_conversation_with_a_farewell(monkeypatch):
    monkeypatch.setattr(server, 'speech_to_text', lambda p: 'Talk to you later!')
    monkeypatch.setattr(
        server,
        'speech_to_wav',
        lambda text, out, directory: out.write_bytes(b'RIFF-farewell'),
    )
    called = False

    def fail_if_called(text):
        nonlocal called
        called = True
        raise AssertionError('exit phrase must not reach Hermes')

    monkeypatch.setattr(server, 'ask_hermes', fail_if_called)
    response = client.post('/turn', headers=KEY, content=make_wav())
    assert response.status_code == 202
    jid = response.json()['id']
    status = client.get(f'/jobs/{jid}', headers=KEY).json()
    assert status['state'] == 'conversation_end'
    assert status['transcript'] == 'Talk to you later!'
    assert status['answer'] == 'Goodbye. Talk to you later!'
    assert called is False
    audio = client.get(f'/jobs/{jid}/audio', headers=KEY)
    assert audio.status_code == 200
    assert audio.content == b'RIFF-farewell'


def test_exit_phrase_matching_is_conservative():
    assert server.is_conversation_end_phrase('talk to you later') is True
    assert server.is_conversation_end_phrase('Talk to you later!') is True
    assert server.is_conversation_end_phrase('I want to talk to you later about this') is False
    assert server.is_conversation_end_phrase('talk to you later about the project') is False


def test_cancel_job_is_authenticated_and_idempotent():
    directory = tempfile.TemporaryDirectory(prefix='arlo-test-cancel-')
    jid = 'cancel-test-job'
    server.jobs[jid] = server.Job(id=jid, directory=directory)
    try:
        assert client.post(f'/jobs/{jid}/cancel').status_code == 401
        response = client.post(f'/jobs/{jid}/cancel', headers=KEY)
        assert response.status_code == 200
        assert response.json()['state'] == 'cancelled'
        repeated = client.post(f'/jobs/{jid}/cancel', headers=KEY)
        assert repeated.status_code == 200
        assert repeated.json()['state'] == 'cancelled'
        status = client.get(f'/jobs/{jid}', headers=KEY).json()
        assert status['state'] == 'cancelled'
        assert client.get(f'/jobs/{jid}/audio', headers=KEY).status_code == 409
    finally:
        server.jobs.pop(jid, None)
        directory.cleanup()


def test_cancelled_job_never_starts_transcription(monkeypatch):
    directory = tempfile.TemporaryDirectory(prefix='arlo-test-cancel-run-')
    jid = 'cancel-before-run'
    server.jobs[jid] = server.Job(id=jid, directory=directory, state='cancelled')
    called = False

    def fail_if_called(path):
        nonlocal called
        called = True
        raise AssertionError('cancelled job must not transcribe')

    monkeypatch.setattr(server, 'speech_to_text', fail_if_called)
    try:
        server.run_job(jid, Path(directory.name) / 'input.wav')
        assert called is False
        assert server.jobs[jid].state == 'cancelled'
    finally:
        server.jobs.pop(jid, None)
        directory.cleanup()


def test_full_turn_and_audio_download(monkeypatch):
    monkeypatch.setattr(server, 'speech_to_text', lambda p: 'What is your name?')

    def fake_tts(text, out, directory):
        with wave.open(str(out), 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(b'\x00' * (16000 * 4))

    monkeypatch.setattr(server, 'speech_to_wav', fake_tts)
    response = client.post('/turn', headers=KEY, content=make_wav())
    assert response.status_code == 202
    jid = response.json()['id']
    status = client.get(f'/jobs/{jid}', headers=KEY).json()
    assert status['state'] == 'ready', status
    assert status['transcript'] == 'What is your name?'
    assert 'I heard you say' in status['answer']
    audio = client.get(f'/jobs/{jid}/audio', headers=KEY)
    assert audio.status_code == 200
    assert audio.content[:4] == b'RIFF'
    assert client.get(f'/jobs/{jid}/audio').status_code == 401


def test_full_turn_exposes_sentence_audio_segments(monkeypatch):
    monkeypatch.setattr(server, 'speech_to_text', lambda p: 'Tell me two things.')
    def fake_hermes(text, on_sentence=None):
        result = 'First sentence. Second sentence.'
        if on_sentence:
            for sentence in server.split_spoken_sentences(result):
                on_sentence(sentence)
        return result

    monkeypatch.setattr(server, 'ask_hermes', fake_hermes)

    def fake_tts(text, out, directory):
        with wave.open(str(out), 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(b'\x00' * (16000 * 4))

    monkeypatch.setattr(server, 'speech_to_wav', fake_tts)
    response = client.post('/turn', headers=KEY, content=make_wav())
    assert response.status_code == 202
    jid = response.json()['id']
    status = client.get(f'/jobs/{jid}', headers=KEY).json()
    assert status['state'] == 'ready', status
    assert status['segments_ready'] == 2
    assert status['segments_total'] == 2
    assert client.get(f'/jobs/{jid}/audio?segment=1', headers=KEY).status_code == 200


def test_phrase_streaming_prefers_natural_clause_boundaries():
    text = (
        "Well now, sugar, that's a mighty fine question. "
        "Give me just a minute, and we'll get it sorted out."
    )

    assert server.split_spoken_phrases(text) == [
        "Well now, sugar, that's a mighty fine question.",
        "Give me just a minute,",
        "and we'll get it sorted out.",
    ]


def test_phrase_streaming_emits_incremental_units_without_dropping_text():
    first, pending = server.consume_spoken_segments(
        "Well now, sugar, that's a mighty fine question. Give me",
        final=False,
    )

    assert first == ["Well now, sugar, that's a mighty fine question."]
    assert pending == "Give me"

    second, pending = server.consume_spoken_segments(
        pending + " just a minute, and we'll get it sorted out.",
        final=True,
    )

    assert second == [
        "Give me just a minute,",
        "and we'll get it sorted out.",
    ]
    assert pending == ""


def test_full_turn_reports_pipeline_timings(monkeypatch):
    monkeypatch.setattr(server, 'speech_to_text', lambda p: 'Give me a quick update.')
    monkeypatch.setattr(
        server,
        'speech_to_wav',
        lambda text, out, directory: out.write_bytes(b'RIFF-timed'),
    )

    response = client.post('/turn', headers=KEY, content=make_wav())
    assert response.status_code == 202
    jid = response.json()['id']
    status = client.get(f'/jobs/{jid}', headers=KEY).json()
    timings = status['timings']

    assert status['state'] == 'ready'
    assert timings['stt_ms'] >= 0
    assert timings['hermes_ms'] >= 0
    assert timings['first_audio_ready_ms'] >= 0
    assert timings['first_phrase_tts_start_ms'] >= 0
    assert timings['total_ms'] >= timings['first_audio_ready_ms']
