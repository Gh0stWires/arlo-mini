import os
import tempfile
from pathlib import Path

os.environ['DEVICE_KEY'] = 'test-device-secret-not-for-production'
os.environ['MOCK_HERMES'] = 'true'

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
import server
from fastapi.testclient import TestClient


KEY = {"X-Device-Key": "test-device-secret-not-for-production"}
client = TestClient(server.app)


class FakePcmSession:
    def __init__(self):
        self.cancelled = False

    def iter_chunks(self):
        yield b"pcm-one"
        yield b"pcm-two"

    def cancel(self):
        self.cancelled = True


def test_pcm_stream_replays_incremental_pcm_and_claims_once():
    directory = tempfile.TemporaryDirectory(prefix="arlo-test-pcm-")
    jid = "pcm-stream-test"
    session = FakePcmSession()
    server.jobs[jid] = server.Job(
        id=jid,
        directory=directory,
        state="streaming",
        pcm_stream_ready=True,
        pcm_stream_session=session,
    )
    try:
        response = client.get(f"/jobs/{jid}/pcm", headers=KEY)
        assert response.status_code == 200
        assert response.content == b"pcm-onepcm-two"
        assert response.headers["content-type"] == "application/octet-stream"
        assert server.jobs[jid].pcm_stream_claimed is True

        repeated = client.get(f"/jobs/{jid}/pcm", headers=KEY)
        assert repeated.status_code == 409
    finally:
        server.jobs.pop(jid, None)
        directory.cleanup()


def test_pcm_stream_requires_authentication():
    directory = tempfile.TemporaryDirectory(prefix="arlo-test-pcm-auth-")
    jid = "pcm-auth-test"
    server.jobs[jid] = server.Job(id=jid, directory=directory)
    try:
        assert client.get(f"/jobs/{jid}/pcm").status_code == 401
    finally:
        server.jobs.pop(jid, None)
        directory.cleanup()
