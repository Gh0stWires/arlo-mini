#pragma once

#include <stdint.h>

namespace arlo_voice {

enum class CaptureDecision : uint8_t {
    WaitingForSpeech,
    CapturingSpeech,
    Finished,
    InitialSilenceTimeout,
    MaximumDuration,
    Cancelled,
};

struct VoiceActivityConfig {
    uint16_t frameMs = 20;
    uint16_t startFrames = 2;
    uint16_t silenceHangoverMs = 750;
    uint16_t initialSilenceMs = 1800;
    uint32_t maximumUtteranceMs = 30000;
    uint16_t minimumSpeechRms = 700;
    uint8_t noiseMultiplier = 3;
};

struct VoiceFrame {
    uint32_t elapsedMs;
    uint16_t rms;
};

class VoiceActivity {
public:
    explicit VoiceActivity(const VoiceActivityConfig &config = VoiceActivityConfig());

    CaptureDecision push(const VoiceFrame &frame);
    void cancel();

    bool speechStarted() const { return _speechStarted; }
    bool finished() const { return _finished; }
    uint32_t speechStartMs() const { return _speechStartMs; }
    uint32_t trailingSilenceMs() const { return _trailingSilenceMs; }
    uint16_t noiseFloorRms() const { return _noiseFloorRms; }

private:
    bool isSpeech(uint16_t rms) const;

    VoiceActivityConfig _config;
    bool _speechStarted = false;
    bool _finished = false;
    uint16_t _speechFrames = 0;
    uint16_t _noiseFloorRms = 0;
    uint32_t _speechStartMs = 0;
    uint32_t _lastSpeechMs = 0;
    uint32_t _trailingSilenceMs = 0;
    CaptureDecision _decision = CaptureDecision::WaitingForSpeech;
};

}  // namespace arlo_voice
