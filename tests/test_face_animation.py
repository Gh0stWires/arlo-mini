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
    if not has_previous or caption_changed or previous.state != current.state:
        return {"full"}
    dirty = set()
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


def test_caption_change_requires_full_static_render():
    assert dirty_regions(FaceModel(), FaceModel(), True, True) == {"full"}


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
    assert "clearFaceRegion(43, 93, 154, 78, fill);" in source
    assert "clearFaceRegion(94, 188, 52, 43, fill);" in source
