#include "face_animation.h"

namespace arloface {

FaceDirtyRegion diff(const FaceModel &previous, const FaceModel &next,
                     bool hasPrevious, bool captionChanged) {
    FaceDirtyRegion dirty;
    if (!hasPrevious || previous.state != next.state) {
        dirty.full = true;
        return dirty;
    }

    dirty.caption = captionChanged;
    dirty.eyes = previous.gaze != next.gaze ||
                 previous.blinkPhase != next.blinkPhase;
    dirty.mouth = previous.mouthFrame != next.mouthFrame;
    dirty.accent = previous.accentFrame != next.accentFrame;
    return dirty;
}

}  // namespace arloface
