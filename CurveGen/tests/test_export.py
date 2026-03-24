"""
tests/test_export.py — Unit tests for export.py preset read/write.
"""
import json
import os
import tempfile
import pytest
from curvegen.export import write_preset, read_preset, preset_to_gains


SAMPLE_GAINS = [2.5, 1.0, -1.5, 0.0, 0.5, -2.0, 1.0, 0.0, -0.5, 1.5]
SAMPLE_PREAMP = -3.0


class TestWritePreset:

    def test_writes_valid_json(self, tmp_path):
        path = str(tmp_path / "test.json")
        write_preset(path, "Test", SAMPLE_GAINS, SAMPLE_PREAMP)
        with open(path) as f:
            data = json.load(f)
        assert data["version"] == 1
        assert data["name"] == "Test"
        assert len(data["bands"]) == 10

    def test_gains_roundtrip(self, tmp_path):
        path = str(tmp_path / "test.json")
        write_preset(path, "RT", SAMPLE_GAINS, SAMPLE_PREAMP)
        preset = read_preset(path)
        gains, preamp = preset_to_gains(preset)
        assert abs(preamp - SAMPLE_PREAMP) < 0.01
        for got, expected in zip(gains, SAMPLE_GAINS):
            assert abs(got - expected) < 0.01

    def test_mismatched_lengths_raises(self, tmp_path):
        path = str(tmp_path / "bad.json")
        with pytest.raises(ValueError, match="length"):
            write_preset(path, "Bad", SAMPLE_GAINS[:5], 0.0)

    def test_creates_parent_dir(self, tmp_path):
        path = str(tmp_path / "subdir" / "preset.json")
        write_preset(path, "Dir", SAMPLE_GAINS, 0.0)
        assert os.path.exists(path)


class TestReadPreset:

    def test_missing_key_raises(self, tmp_path):
        path = str(tmp_path / "bad.json")
        with open(path, "w") as f:
            json.dump({"version": 1, "name": "x"}, f)
        with pytest.raises(ValueError, match="missing keys"):
            read_preset(path)

    def test_wrong_band_count_raises(self, tmp_path):
        path = str(tmp_path / "bad2.json")
        with open(path, "w") as f:
            json.dump({
                "version": 1, "name": "x", "preamp_db": 0,
                "bands": [{"hz": 1000, "gain_db": 0}]
            }, f)
        with pytest.raises(ValueError, match="10 bands"):
            read_preset(path)
