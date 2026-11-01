from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "benchmark_pcm_amplitude.py"
spec = spec_from_file_location("benchmark_pcm_amplitude", MODULE_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError("unable to load amplitude benchmark module")
benchmark = module_from_spec(spec)
spec.loader.exec_module(benchmark)


def test_adaptive_stream_has_gate_and_dynamic_range():
    levels = [0.0] * 10 + [180.0] * 10 + [2200.0] * 10 + [600.0] * 10 + [0.0] * 10
    amplitudes = benchmark.adaptive_stream(levels)

    assert amplitudes[:10] == [0] * 10
    assert max(amplitudes) > 80
    assert amplitudes[-1] < amplitudes[29]
    assert all(0 <= value <= 255 for value in amplitudes)


def test_frame_distribution_preserves_rising_and_falling_step():
    result = benchmark.frame_distribution([0, 20, 100, 20, 0])

    assert sum(result["counts"]) == 5
    assert result["counts"][0] >= 1
    assert result["counts"][2] >= 1
    assert result["counts"][4] >= 1


def test_fixed_divisor_is_distinct_from_adaptive_stream():
    levels = [128.0, 512.0, 1024.0, 2048.0]

    assert benchmark.old_fixed_divisor(levels) != benchmark.adaptive_stream(levels)
