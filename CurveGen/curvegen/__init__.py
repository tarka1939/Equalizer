"""
curvegen — Acoustic room-correction EQ curve generator.

Public API
----------
measurement.load_wav(path)            → (freqs, magnitude_db, sample_rate)
measurement.generate_sweep_response() → (freqs, magnitude_db, sample_rate)
flatten.compute_correction(freqs, magnitude_db, bands, ...)
                                      → (gains_db, preamp_db)
export.write_preset(path, name, bands_hz, gains_db, preamp_db)
eqapo_export.write_eqapo_config(path, gains_db, band_hz, preamp_db, q)
                                      → offline Equalizer APO config export,
                                        for real-world validation of the
                                        curve-generation output (see
                                        curvegen/eqapo_export.py)
"""
