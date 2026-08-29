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
All ten values must be finite. A `NaN` or `Infinity` is rejected: it would
otherwise reach the biquad, land in the filter history, and make every
subsequent output sample `NaN` forever — no later valid curve can recover
from that.

### `set_preamp`
Set global preamp gain (dB). Must be a finite number in the range
**[-60, +20] dB**; anything outside that, or a `NaN`/`Infinity`, is rejected
with an error. The bound exists because the audio path turns this value into
`pow(10, gain_db / 20)`: a large enough input overflows to `+Inf`, and
`Inf * 0` (a silent sample) is `NaN`, which then propagates through the rest
of the chain.
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
handoff as `set_bands`: the IPC thread stages the taps and a non-RT **control
thread** in the audio backend picks them up and installs them. (It is not
applied on the audio callback — computing the filter spectrum runs a full FFT
and used to allocate; see `ARCHITECTURE.md` section 4.1.) Changes therefore
take effect within a few milliseconds rather than on the very next callback.

`taps` must be non-empty, no longer than 4096 samples
(`EqState::kMaxFirTaps`), and all values finite; oversized, empty, and
non-finite arrays are rejected with an error. Once set, the FIR stage becomes
active:
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
{ "ok": false, "error": "invalid gains_db array" }
```
The `error` string is JSON-escaped. Some messages echo part of the request
back (`unknown command: <cmd>`), so quotes, backslashes and control
characters in the client's own input are escaped rather than spliced in raw —
splicing them produced replies that were not parseable JSON.

Connections are also dropped if a single command line exceeds
`IpcServer::kMaxLineBytes` (1 MiB) without a terminating newline.

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
