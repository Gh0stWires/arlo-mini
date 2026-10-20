from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VOICE_HEADER = (ROOT / "firmware" / "src" / "voice_activity.h").read_text()
VOICE_SOURCE = (ROOT / "firmware" / "src" / "voice_activity.cpp").read_text()
FIRMWARE_SOURCE = (ROOT / "firmware" / "src" / "main.cpp").read_text()


def test_vad_model_has_speech_start_and_silence_states():
    assert "WaitingForSpeech" in VOICE_HEADER
    assert "CapturingSpeech" in VOICE_HEADER
    assert "InitialSilenceTimeout" in VOICE_HEADER
    assert "MaximumDuration" in VOICE_HEADER
    assert "Cancelled" in VOICE_HEADER
    assert "_speechFrames" in VOICE_SOURCE
    assert "_trailingSilenceMs" in VOICE_SOURCE


def test_vad_model_requires_consecutive_speech_frames():
    assert "startFrames" in VOICE_HEADER
    assert "_speechFrames < _config.startFrames" in VOICE_SOURCE
    assert "_speechStartMs" in VOICE_SOURCE


def test_vad_allows_a_natural_pause_before_finishing():
    assert "silenceHangoverMs = 1400" in VOICE_HEADER
    assert "_trailingSilenceMs >= _config.silenceHangoverMs" in VOICE_SOURCE


def test_vad_uses_lower_threshold_for_quiet_continuation():
    assert "minimumContinuationRms" in VOICE_HEADER
    assert "continuationNoiseMultiplier" in VOICE_HEADER
    assert "isSpeech(frame.rms, _speechStarted)" in VOICE_SOURCE
    assert "minimumContinuationRms" in VOICE_SOURCE


def test_firmware_recorder_uses_preroll_and_patches_wav_length():
    assert "VAD_PREROLL_FRAMES = 12" in FIRMWARE_SOURCE
    assert "heap_caps_malloc(prerollBytes, MALLOC_CAP_SPIRAM)" in FIRMWARE_SOURCE
    assert "lv_fs_seek(&file, 0, LV_FS_SEEK_SET)" in FIRMWARE_SOURCE
    assert "fillWavHeader(header, dataBytes)" in FIRMWARE_SOURCE


def test_voice_turn_no_longer_uses_fixed_duration_recorder():
    assert "recordUntilSpeechEnds()" in FIRMWARE_SOURCE
    assert "music.recordSaveToTFCard" not in FIRMWARE_SOURCE
    assert "VAD capture complete bytes=" in FIRMWARE_SOURCE
