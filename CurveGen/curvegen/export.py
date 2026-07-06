"""
export.py — Write an EQ preset JSON file from computed correction gains.
"""
from __future__ import annotations

import json
import os
from typing import Optional, Sequence

from .flatten import DEFAULT_BAND_HZ

SCHEMA_VERSION = 1


def write_preset(
    path: str,
    name: str,
    gains_db: Sequence[float],
    preamp_db: float = 0.0,
    band_hz: Optional[Sequence[float]] = None,
    description: str = "",
) -> None:
    """
    Write a preset JSON file.

    Parameters
    ----------
    path      : output file path (e.g. "room_correction.json")
    name      : human-readable preset name
    gains_db  : per-band gains in dB (10 values)
    preamp_db : global preamp in dB
    band_hz   : band centre frequencies (default: standard 10-band)
    description : optional preset description string
    """
    if band_hz is None:
        band_hz = DEFAULT_BAND_HZ
    if len(gains_db) != len(band_hz):
        raise ValueError(
            f"gains_db length ({len(gains_db)}) must match band_hz length ({len(band_hz)})"
        )

    preset: dict = {
        "version":    SCHEMA_VERSION,
        "name":       name,
        "preamp_db":  round(float(preamp_db), 2),
        "bands": [
            {
                "hz":      float(hz),
                "gain_db": round(float(g), 2),
            }
            for hz, g in zip(band_hz, gains_db)
        ],
    }
    if description:
        preset["description"] = description

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(preset, f, indent=2)
        f.write("\n")

    print(f"[export] Preset saved to {path}")


def read_preset(path: str) -> dict:
    """
    Load and return a preset JSON file as a dict.
    Raises ValueError if the file is missing required keys.
    """
    with open(path, encoding="utf-8") as f:
        data = json.load(f)

    required = {"version", "name", "preamp_db", "bands"}
    missing = required - data.keys()
    if missing:
        raise ValueError(f"Preset missing keys: {missing}")
    if len(data["bands"]) != 10:
        raise ValueError(f"Expected 10 bands, got {len(data['bands'])}")

    return data


def preset_to_gains(preset: dict) -> tuple[list[float], float]:
    """
    Extract (gains_db, preamp_db) from a loaded preset dict.
    """
    gains   = [b["gain_db"] for b in preset["bands"]]
    preamp  = float(preset["preamp_db"])
    return gains, preamp
