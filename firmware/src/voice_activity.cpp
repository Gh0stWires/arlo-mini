#include "voice_activity.h"

namespace arlo_voice {

VoiceActivity::VoiceActivity(const VoiceActivityConfig &config)
    : _config(config) {}

bool VoiceActivity::isSpeech(uint16_t rms) const {
    const uint32_t adaptiveThreshold =
        static_cast<uint32_t>(_noiseFloorRms) * _config.noiseMultiplier;
    const uint32_t threshold = adaptiveThreshold > _config.minimumSpeechRms
        ? adaptiveThreshold
        : _config.minimumSpeechRms;
    return rms >= threshold;
}

CaptureDecision VoiceActivity::push(const VoiceFrame &frame) {
    if (_finished) return _decision;

    if (frame.elapsedMs >= _config.maximumUtteranceMs) {
        _finished = true;
        _decision = _speechStarted
            ? CaptureDecision::MaximumDuration
            : CaptureDecision::InitialSilenceTimeout;
        return _decision;
    }

    const bool speech = isSpeech(frame.rms);
    if (!_speechStarted) {
        if (!speech) {
            // Slowly track ambient noise without allowing a loud frame to
            // raise the floor enough to hide the beginning of speech.
            const uint16_t limited = frame.rms < _config.minimumSpeechRms
                ? frame.rms
                : _config.minimumSpeechRms;
            _noiseFloorRms = static_cast<uint16_t>(
                (_noiseFloorRms * 7U + limited) / 8U);
            _speechFrames = 0;
            if (frame.elapsedMs >= _config.initialSilenceMs) {
                _finished = true;
                _decision = CaptureDecision::InitialSilenceTimeout;
                return _decision;
            }
            _decision = CaptureDecision::WaitingForSpeech;
            return _decision;
        }

        ++_speechFrames;
        if (_speechFrames < _config.startFrames) {
            _decision = CaptureDecision::WaitingForSpeech;
            return _decision;
        }

        _speechStarted = true;
        _speechStartMs = frame.elapsedMs -
            static_cast<uint32_t>(_config.startFrames - 1) * _config.frameMs;
        _lastSpeechMs = frame.elapsedMs;
        _trailingSilenceMs = 0;
        _decision = CaptureDecision::CapturingSpeech;
        return _decision;
    }

    if (speech) {
        _lastSpeechMs = frame.elapsedMs;
        _trailingSilenceMs = 0;
        _decision = CaptureDecision::CapturingSpeech;
        return _decision;
    }

    _trailingSilenceMs = frame.elapsedMs - _lastSpeechMs;
    if (_trailingSilenceMs >= _config.silenceHangoverMs) {
        _finished = true;
        _decision = CaptureDecision::Finished;
        return _decision;
    }

    _decision = CaptureDecision::CapturingSpeech;
    return _decision;
}

void VoiceActivity::cancel() {
    _finished = true;
    _decision = CaptureDecision::Cancelled;
}

}  // namespace arlo_voice
