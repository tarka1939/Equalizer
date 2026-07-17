# IPC Protocol — Equalizer Daemon

The daemon exposes a JSON-line protocol over:
- **Linux**: Unix domain socket at `/tmp/eq-daemon.sock`
- **Windows**: Named pipe `\\.\pipe\eq-daemon`

Each message is a single UTF-8 JSON object followed by `\n`.

---

## Client → Daemon

### `set_bands`
Set all 10 band gains (dB). Changes are applied immediately (RT-safe atomic swap).
```json
{ "cmd": "set_bands", "gains_db": [0.0, 1.5, -2.0, 0.0, 0.0, 3.0, 0.0, 0.0, -1.0, 0.0] }
```

### `set_preamp`
Set global preamp gain (dB).
```json
{ "cmd": "set_preamp", "gain_db": -3.0 }
```

### `load_preset`
Load a preset from a JSON file path.
```json
{ "cmd": "load_preset", "path": "/home/user/flat_room.json" }
```

### `set_enabled`
Enable or bypass the equalizer.
```json
{ "cmd": "set_enabled", "enabled": true }
```

### `set_fir`
Set the FIR (room-correction) impulse response taps. Applied via the same
non-RT → RT handoff as `set_bands` (pending buffer + atomic dirty flag,
consumed on the next audio callback). `taps` must be non-empty and no
longer than 4096 samples (`EqState::kMaxFirTaps`); oversized or empty
arrays are rejected with an error. Once set, the FIR stage becomes active:
see `DSP::EqPipeline` in `ARCHITECTURE.md` §2.4 for the FIR/IIR execution
order (FIR runs first, then IIR, when both are configured).
```json
{ "cmd": "set_fir", "taps": [0.5, 0.3, -0.1] }
```

### `clear_fir`
Disable the FIR stage and fall back to IIR-only (or passthrough, if no
bands are configured either).
```json
{ "cmd": "clear_fir" }
```

### `get_state`
Query full current state.
```json
{ "cmd": "get_state" }
```

---

## Daemon → Client

### State response (to `get_state`)
```json
{
  "gains_db": [0.0, 1.5, -2.0, 0.0, 0.0, 3.0, 0.0, 0.0, -1.0, 0.0],
  "preamp_db": -3.0,
  "enabled": true,
  "sample_rate": 48000,
  "channels": 2,
  "fir_length": 0
}
```
`fir_length` is the number of taps currently applied by `set_fir` (`0` if
no FIR is configured, i.e. `clear_fir` was called or it was never set).

### Acknowledgement (to mutating commands)
```json
{ "ok": true }
```

### Error response
```json
{ "ok": false, "error": "Invalid gains array length" }
```

---

## Band Centre Frequencies (standard 10-band)

| Band | Hz    |
|------|-------|
| 0    | 31    |
| 1    | 62    |
| 2    | 125   |
| 3    | 250   |
| 4    | 500   |
| 5    | 1000  |
| 6    | 2000  |
| 7    | 4000  |
| 8    | 8000  |
| 9    | 16000 |
