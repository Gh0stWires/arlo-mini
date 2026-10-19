"""Offline protocol tests: no Hermes instance, Whisper model, or TTS needed."""

import importlib
import io
import os
import sys
import tempfile
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


def test_talk_to_you_later_ends_conversation_without_hermes(monkeypatch):
    monkeypatch.setattr(server, 'speech_to_text', lambda p: 'Talk to you later!')
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
    assert called is False
    assert client.get(f'/jobs/{jid}/audio', headers=KEY).status_code == 409


def test_exit_phrase_matching_is_conservative():
    assert server.is_conversation_end_phrase('talk to you later') is True
    assert server.is_conversation_end_phrase('Talk to you later!') is True
    assert server.is_conversation_end_phrase('I want to talk to you later about this') is False
    assert server.is_conversation_end_phrase('talk to you later about the project') is False


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
