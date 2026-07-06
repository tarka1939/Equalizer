# APO installation attempts report (Win11 + Realtek UAPO stack)

## Goal
Implement a custom 10‑band equalizer as a Windows Audio Processing Object (APO) and install it so it processes audio system‑wide without relying on third‑party audio processing tools.

## Environment summary
- OS: Windows 11 (physical machine)
- Audio stack involved:
  - Realtek UAPO: `RltkAPOU642.dll` (loaded by `audiodg.exe`)
  - Nahimic APO: `NahimicAPO4.dll` (loaded by `audiodg.exe`)
- Security/features encountered during the work:
  - Secure Boot initially enabled and later disabled
  - Virtualization-based security indicators were present (DeviceGuard service `{2}` observed earlier)
  - Registry ACL enforcement on endpoint effect keys (writes blocked even as SYSTEM)

## Evidence collected
- `audiodg.exe` module list showed OEM APOs are loaded (Realtek UAPO and Nahimic APO).
- File-based trace (`C:\driver\eq_apo.txt`), used to detect COM activation attempts, was not created during playback → audio engine did not attempt to instantiate the custom APO.
- Endpoint `FxProperties` contained additional list/mode properties beyond `StreamEffectClsid`/`ModeEffectClsid`:
  - `{d3993a3f-99c2-4402-b5ec-a92a0367664b},5/6/7/11/12` were `MultiString` lists containing an existing CLSID.
  - Several mode-related GUID groups (e.g., `{670173E3-78CF-11E5-A837-0800200C9A66}`) were present, indicating a more complex effects chain model.

## Approach 1 — COM registration and APO catalog registration
### What was done
- Registered the DLL as a COM in-proc server (`regsvr32`) which creates:
  - `HKLM\SOFTWARE\Classes\CLSID\{APO-CLSID}\InprocServer32`
- Registered the APO in the audio engine catalog:
  - `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\{APO-CLSID}`

### Expected
Once referenced by an endpoint chain, `audiodg.exe` should load the DLL and instantiate the APO.

### Result
- Registration succeeded.
- The APO was not instantiated during playback.

### Why it didn’t work
On Win11 + OEM UAPO stacks, being a registered APO is not sufficient. The endpoint chain must accept and reference it using the mechanism employed by the driver stack.

## Approach 2 — Set endpoint Stream/Mode effect CLSIDs via MMDevices `FxProperties`
### What was done
Attempted to set the endpoint properties:
- `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5` (StreamEffectClsid)
- `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6` (ModeEffectClsid)
under:
- `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{endpoint}\FxProperties`

### Expected
The audio engine would load the custom APO for that endpoint.

### Result
Two blockers were observed:
- Some registry writes were blocked with access denied (“Requested registry access is not allowed”).
- When the values were successfully set (via INF-based install), `audiodg.exe` still did not load the custom DLL.

### Why it didn’t work
The OEM chain (Realtek UAPO + Nahimic) appears to build/evaluate effects using other properties/mode lists. The engine did not attempt COM activation even when `StreamEffectClsid`/`ModeEffectClsid` pointed to the custom CLSID.

## Approach 3 — Elevation to SYSTEM to bypass registry ACL
### What was done
- Spawned a SYSTEM PowerShell using Sysinternals PsExec (`psexec -i -s`).
- Retried registry edits under SYSTEM.

### Expected
SYSTEM would be able to modify endpoint effect chain keys.

### Result
Writes to the relevant endpoint chain properties were still blocked.

### Why it didn’t work
The relevant portions of the endpoint configuration are protected by stronger ACL/policy (likely driver-defined security descriptors). This is not a simple UAC/admin permission issue.

## Approach 4 — Test-only INF via `InstallHinfSection` (copy + register + set endpoint)
### What was done
- Created a test INF `installer/EqualizerTest.inf` that:
  - copies `Equalizer.dll`
  - uses `RegisterDlls` to call COM registration
  - writes endpoint `FxProperties` values
- Installed using:
  - `rundll32 setupapi.dll,InstallHinfSection ...`

### Expected
Automate the registry + COM setup.

### Result
- Install actions executed (file copy / COM registration / values present).
- `audiodg.exe` still did not load the APO.

### Why it didn’t work
The issue was not the installation method; it was that the active driver stack did not honor the overridden entries as an instantiation source.

## Approach 5 — Attempt a Realtek device-associated installation with `pnputil`
### What was done
- Created an INF targeting the Realtek hardware ID and attempted:
  - `pnputil /add-driver ... /install`

### Result
- `pnputil` failed with generic INF errors.
- SetupAPI and `infverif` showed the INF was not a valid extension driver package.

### Why it didn’t work
A valid Extension INF on Win10/11 must follow strict rules:
- Must declare `ExtensionId` and typically `PnpLockdown=1`.
- Must be device-scoped (`HKR`), not write arbitrary `HKLM\...\MMDevices` keys.
- Must not use legacy directives such as `RegisterDlls`/`UnregisterDlls`.
- Must obey packaging/isolation constraints.

## Approach 6 — Build a compliant Extension INF skeleton (installed successfully)
### What was done
- Replaced the previous INF with a compliant Extension INF skeleton:
  - Added `ExtensionId`, `PnpLockdown`, `CatalogFile`.
  - Restricted registry modifications to `HKR`.
  - Produced and signed a `.cat` for the package.
  - Installed successfully with `pnputil`.

### Result
- Package installed correctly as a device extension.
- Audio processing remained unchanged.

### Why it didn’t enable processing
A compliant Extension INF cannot directly:
- write endpoint `MMDevices` effect chain keys,
- perform COM self-registration (`RegisterDlls`),
- or override OEM effect lists.

So it validated the extension-driver packaging path but did not (and cannot) inject the custom APO into the OEM-controlled chain.

## Approach 7 — Attempt to insert into OEM effect list properties
### What was attempted
- Identified `MultiString` lists under:
  - `{d3993a3f-99c2-4402-b5ec-a92a0367664b},5/6/7/11/12`
- Planned to append the custom CLSID to these lists.

### Result
- Registry writes were blocked (“Requested registry access is not allowed”) even under SYSTEM.

### Why it didn’t work
Those list properties are also protected, preventing insertion into the chain via registry edits.

## Conclusion: feasibility assessment
The work established that:
- The machine’s active audio chain is OEM-controlled (Realtek UAPO + Nahimic).
- The endpoint effect chain configuration is protected such that it cannot be altered via registry edits, even under SYSTEM.
- Win11 extension-driver packaging rules prevent using a compliant extension INF to perform the required chain/COM modifications.

### Why a native, system-wide APO insertion was not feasible within reasonable scope
Achieving the original goal would require OEM-level integration:
- implementing the official Win11 Audio Effects Discovery model for the specific driver stack,
- packaging and signing with the correct device-scoped registration,
- and potentially using undocumented or OEM-only interfaces (e.g., Realtek UAPO APIs).

This is disproportionate compared to the project’s initial scope (writing a custom APO DLL) because it shifts effort from DSP/APO implementation to OEM driver integration and Windows audio platform compliance.

## Artifacts in this repo related to the attempts
- `LOCAL_TEST_GUIDE.md` — installation/testing/uninstall command set used during experiments.
- `installer/EqualizerTest.inf` — test-only INF that copies/registers and sets endpoint properties.
- `installer/EqualizerRealtekExtension.inf` — compliant extension INF skeleton (device-associated package marker).
