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


def test_conversation_uses_a_session_deadline_not_fixed_followups():
    assert "CONVERSATION_SESSION_MS" in FIRMWARE_SOURCE
    assert "while (millis() - sessionStarted < CONVERSATION_SESSION_MS)" in FIRMWARE_SOURCE
    assert "MAX_FOLLOWUPS" not in FIRMWARE_SOURCE


def test_firmware_supports_wake_word_interruption_path():
    assert "Wake-word interruption requested during job" in FIRMWARE_SOURCE
    assert "Wake-word interruption requested during playback" not in FIRMWARE_SOURCE
    assert "void playback()" in FIRMWARE_SOURCE


def test_voice_turn_no_longer_uses_fixed_duration_recorder():
    assert "recordUntilSpeechEnds()" in FIRMWARE_SOURCE
    assert "music.recordSaveToTFCard" not in FIRMWARE_SOURCE
    assert "VAD capture complete bytes=" in FIRMWARE_SOURCE


def test_playback_completion_does_not_poll_the_sdk_audio_state():
    playback_start = FIRMWARE_SOURCE.index("void playback()")
    voice_turn_start = FIRMWARE_SOURCE.index("bool voiceTurn", playback_start)
    playback_source = FIRMWARE_SOURCE[playback_start:voice_turn_start]

    assert "k10.getPlayMusicState()" not in playback_source
    assert "minimumPlaybackMs" in playback_source
    assert "music.stopPlayAudio()" in playback_source
    assert "nextMouthUpdate = now + 90" in playback_source
    assert "peakAverage" in FIRMWARE_SOURCE
    assert "(average * 255U) / peakAverage" in FIRMWARE_SOURCE


def test_firmware_fetches_and_plays_sentence_segments_as_they_arrive():
    assert "segments_ready" in FIRMWARE_SOURCE
    assert "segments_total" in FIRMWARE_SOURCE
    assert "stream_complete" in FIRMWARE_SOURCE
    assert "?segment=" in FIRMWARE_SOURCE
    assert "fetchAudio(job, segment)" in FIRMWARE_SOURCE


def test_firmware_does_not_flash_to_thinking_between_audio_segments():
    voice_turn_start = FIRMWARE_SOURCE.index("bool voiceTurn")
    voice_turn_source = FIRMWARE_SOURCE[voice_turn_start:]
    poll_start = FIRMWARE_SOURCE.index("String pollJob")
    poll_source = FIRMWARE_SOURCE[poll_start:voice_turn_start]

    assert 'if (segment == 0) {' in voice_turn_source
    assert 'face("thinking", "Getting voice...");' in voice_turn_source
    assert 'face("thinking", segment == 0 ? "Getting voice..." : "Next sentence...");' not in voice_turn_source
    assert 'if (wantedSegment == 0) {' in poll_source
