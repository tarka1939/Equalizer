"""
curvegen — Acoustic room-correction EQ curve generator.

Public API
----------
measurement.load_wav(path)            → (freqs, magnitude_db, sample_rate)
measurement.generate_sweep_response() → (freqs, magnitude_db, sample_rate)
flatten.compute_correction(freqs, magnitude_db, bands, ...)
                                      → (gains_db, preamp_db)
export.write_preset(path, name, bands_hz, gains_db, preamp_db)
"""
