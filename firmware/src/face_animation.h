#pragma once

#include <stdint.h>

namespace arloface {

enum class FaceState : uint8_t {
    Idle = 0,
    Listening,
    Thinking,
    Speaking,
    Error,
};

enum class Gaze : uint8_t {
    Center = 0,
    Left,
    Right,
    Up,
};

struct FaceModel {
    FaceState state = FaceState::Idle;
    Gaze gaze = Gaze::Center;
    uint8_t blinkPhase = 0;
    uint8_t mouthFrame = 0;
    uint8_t accentFrame = 0;
};

struct FaceDirtyRegion {
    bool full = false;
    bool eyes = false;
    bool mouth = false;
    bool accent = false;
    bool caption = false;

    bool any() const {
        return full || eyes || mouth || accent || caption;
    }
};

FaceDirtyRegion diff(const FaceModel &previous, const FaceModel &next,
                     bool hasPrevious, bool captionChanged);

}  // namespace arloface
