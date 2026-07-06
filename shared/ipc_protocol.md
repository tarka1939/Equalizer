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
  "channels": 2
}
```

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
