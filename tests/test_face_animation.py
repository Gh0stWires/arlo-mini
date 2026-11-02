from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path


class State(Enum):
    IDLE = auto()
    LISTENING = auto()
    THINKING = auto()
    SPEAKING = auto()
    ERROR = auto()


class Gaze(Enum):
    CENTER = auto()
    LEFT = auto()
    RIGHT = auto()
    UP = auto()


@dataclass(frozen=True)
class FaceModel:
    state: State = State.IDLE
    gaze: Gaze = Gaze.CENTER
    blink_phase: int = 0
    mouth_frame: int = 0
    accent_frame: int = 0


def dirty_regions(previous, current, has_previous, caption_changed):
    if not has_previous or previous.state != current.state:
        return {"full"}
    dirty = {"caption"} if caption_changed else set()
    if (previous.gaze, previous.blink_phase) != (current.gaze, current.blink_phase):
        dirty.add("eyes")
    if previous.mouth_frame != current.mouth_frame:
        dirty.add("mouth")
    if previous.accent_frame != current.accent_frame:
        dirty.add("accent")
    return dirty


def test_initial_or_state_change_requires_full_render():
    old = FaceModel()
    assert dirty_regions(old, old, False, False) == {"full"}
    assert dirty_regions(old, FaceModel(state=State.THINKING), True, False) == {"full"}


def test_caption_change_invalidates_caption_region_only():
    assert dirty_regions(FaceModel(), FaceModel(), True, True) == {"caption"}


def test_blink_only_invalidates_eyes():
    assert dirty_regions(FaceModel(), FaceModel(blink_phase=1), True, False) == {"eyes"}


def test_mouth_only_invalidates_mouth():
    assert dirty_regions(FaceModel(), FaceModel(mouth_frame=3), True, False) == {"mouth"}


def test_eye_and_mouth_changes_do_not_invalidate_static_layer():
    current = FaceModel(blink_phase=1, mouth_frame=3)
    assert dirty_regions(FaceModel(), current, True, False) == {"eyes", "mouth"}


def test_unchanged_model_requests_no_redraw():
    model = FaceModel()
    assert dirty_regions(model, model, True, False) == set()


def test_firmware_keeps_full_clear_inside_full_renderer_only():
    source = (Path(__file__).resolve().parents[1] / "firmware" / "src" / "main.cpp").read_text()
    assert source.count("canvasClear();") == 1
    assert "if (!dirty.any()) return;" in source
    assert "clearFaceRegion(43, 93, 154, 78, fill);" not in source
    assert "clearEyeRegion(fill);" in source
    assert "drawRoundedRect(54, 101, 52, 74, 26, fill);" in source
    assert "drawRoundedRect(134, 101, 52, 74, 26, fill);" in source
    assert "clearEyeRegion(fill);\n            // Eye bounds overlap" in source
    assert "drawClockTicks();\n            drawEyes(state, next.gaze" in source
    assert "clearFaceRegion(94, 176, 52, 36, fill);" in source
    assert 'String thinkingCaption = "Thinking...";' in source
    assert 'thinkingCaption = "Transcribing...";' in source
    assert 'face("thinking", thinkingCaption, false, 0);' in source

    assert "uint8_t animationFrame = 0;" in source
    assert "faceGaze(state, animationFrame)" in source
    assert "thinkingMouthFrame(animationFrame)" in source
    assert "canvasLine(49, 98, 94, 105, OUTLINE);" not in source
    assert "case 1: return 6;" not in source
    assert "AUDIO_ENVELOPE_BINS" in source
    assert "buildAudioEnvelope" in source
    assert "mouthFrameFromAmplitude" in source
    assert "mouthFrameFromAudio" in source
    assert "AUDIO_ENVELOPE_BINS = 1024" in source
    assert "Playback duration wait complete" in source
    assert "state == \"speaking\"" in source
    assert "canvasCircle(120, 207, 8, OUTLINE" not in source
    assert "settleMouthAfterPlayback" in source
    assert "PCM_FACE_UPDATE_MS = 45" in source
    assert "PCM_FACE_WINDOW_FRAMES = 320" in source
    assert "PCM_FACE_WINDOW_BYTES = PCM_FACE_WINDOW_FRAMES * 4" in source
    assert "PCM_FACE_HEADROOM = 0.70f" in source
    assert "PCM_FACE_NOISE_GATE = 8" in source
    assert "pcmFaceSamplePosition" in source
    assert "pcmFaceEnvelopeSequence" in source
    assert "pcmUpdateAmplitude(item + itemOffset, written" in source
    assert "settleMouthAfterPlayback(pcmFaceFrame);" in source
    assert "uint8_t amplitude = pcmAmplitude(item, itemSize);" not in source
    assert "vTaskDelay(pdMS_TO_TICKS(90));" not in source
    assert "for (uint8_t marker = 0; marker < MARKER_COUNT; ++marker)" in source
    assert "MARKER_COUNT = 12" in source
    assert "drawEyePill(x, 103, 48, 70, 23, OUTLINE);" in source
    assert "drawEyePill(x + 5, 107, 38, 62, 18, EYE_CREAM);" in source
    assert "drawEyePill(x + 3 + lookX, 124 + lookY, 42, 45, 20, OUTLINE);" in source
    assert "void drawNose()" in source
    assert "canvasCircle(120, 155, 8, OUTLINE" in source
    assert "clearFaceRegion(94, 176, 52, 36, fill);" in source
    assert "canvasLine(49, 98, 94, 105, OUTLINE);" not in source
    assert "case 1: return 6;" not in source
    assert "AUDIO_ENVELOPE_BINS" in source


def test_wide_open_mouth_uses_rounded_rectangles():
    source = (Path(__file__).resolve().parents[1] / "firmware" / "src" / "main.cpp").read_text()

    assert "void drawRoundedRect(" in source
    assert "drawRoundedRect(102, 183, 36, 23, 9, OUTLINE);" in source
    assert "drawRoundedRect(108, 189, 24, 11, 5, MOUTH_WINE);" in source
    assert "canvasRectangle(102, 183, 36, 23" not in source
