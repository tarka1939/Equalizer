# Local testing guide (install / enable / verify / disable / uninstall)

> **Before doing any of this by hand:** two automated test projects now cover
> part of what this guide otherwise checks manually — `Equalizer/tests/EqualizerRegistryUtilTests.vcxproj`
> (registry read/write logic, against a `HKEY_CURRENT_USER` scratch key, no
> admin needed) and `Equalizer/tests/EqualizerComExportsTests.vcxproj`
> (`DllGetClassObject`/`DllCanUnloadNow`/class factory behavior, and
> `APOProcess()` called directly with real connection buffers). Both are in
> `Equalizer.sln`. They do **not** replace this guide — actually registering
> the DLL (`DllRegisterServer`/`regsvr32`) and wiring it to a real render
> endpoint's `FxProperties` still needs the manual steps below, since that
> requires admin rights and mutates real `HKEY_LOCAL_MACHINE` state — but
> running the automated tests first is a faster way to catch a broken build
> before going through this whole procedure. See `ARCHITECTURE.md` §9 for
> what each test file covers.

This guide is for testing the `Equalizer.dll` APO on a Windows machine with minimal long‑standing effects.

> Assumptions
> - 64-bit Windows (Win10/Win11)
> - You have built **Release x64** `Equalizer.dll`
> - You will copy it to: `C:\driver\Equalizer.dll`
> - APO CLSID: `{8E259F55-B32B-4FB8-8995-5965798B2C08}`

> [!IMPORTANT]
> **The APO CLSID changed.** It used to be the hand-typed placeholder
> `{12345678-9ABC-4DEF-8011-223344556677}`. If you registered an earlier
> build on this machine, unregister it with the **old** GUID *before*
> installing a new one — otherwise the old `HKLM\SOFTWARE\Classes\CLSID\…`
> and APO-catalog keys are orphaned and still point at the DLL. In an
> elevated CMD:
>
> ```cmd
> reg delete "HKLM\SOFTWARE\Classes\CLSID\{12345678-9ABC-4DEF-8011-223344556677}" /f
> reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\{12345678-9ABC-4DEF-8011-223344556677}" /f
> ```
>
> Also clear any `FxProperties` values on your render endpoint that still
> reference the old GUID (see the uninstall section below), then reboot.

---

## 0) Safety: create backups you can restore

Run in **elevated CMD**:

```cmd
mkdir C:\driver 2>NUL
reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render" "C:\driver\mmdevices_render_backup.reg" /y
reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects" "C:\driver\apo_catalog_backup.reg" /y
reg export "HKLM\SOFTWARE\Classes\CLSID\{8E259F55-B32B-4FB8-8995-5965798B2C08}" "C:\driver\clsid_backup.reg" /y
```

Notes:
- The CLSID export may fail if it doesn’t exist yet; that’s OK.

---

## 1) Install (COM + APO catalog registration)

### 1.1 Copy the DLL
Copy your **Release x64** build to `C:\driver\Equalizer.dll`.

### 1.2 Register
Run in **elevated CMD**:

```cmd
C:\Windows\System32\regsvr32.exe /u "C:\driver\Equalizer.dll"
C:\Windows\System32\regsvr32.exe "C:\driver\Equalizer.dll"
```

### 1.3 Verify COM registration
Run in **PowerShell (admin)**:

```powershell
$clsid = "{8E259F55-B32B-4FB8-8995-5965798B2C08}"
Get-ItemProperty "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32" | Select '(default)', ThreadingModel
```

Expected:
- `(default)` points to `C:\driver\Equalizer.dll`
- `ThreadingModel` is `Both`

### 1.4 Verify APO catalog registration
Run in **PowerShell (admin)**:

```powershell
$clsid = "{8E259F55-B32B-4FB8-8995-5965798B2C08}"
Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\$clsid" |
  Select FriendlyName, MajorVersion, MinorVersion, Flags
```

---

## 2) Enable on an output endpoint (stream/mode FX)

### 2.1 Identify render endpoints
Run in **PowerShell (admin)**:

```powershell
$renderBase = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
Get-ChildItem $renderBase | ForEach-Object {
  $props = "Registry::$($_.Name)\Properties"
  $name = (Get-ItemProperty -Path $props -ErrorAction SilentlyContinue)."{b3f8fa53-0004-438e-9003-51a46e139bfc},6"
  [PSCustomObject]@{ RenderGuid = $_.PSChildName; FriendlyName = $name }
} | Format-Table -AutoSize
```

Pick the `{GUID}` you want to modify (the active output).

### 2.2 Save current FX values (so you can restore)
Run in **PowerShell (admin)** (replace endpoint GUID):

```powershell
$ep = "{312a1cfb-cb49-4bbe-a050-6745dd210145}"
$fx = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$ep\FxProperties"
$k5 = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" # StreamEffectClsid
$k6 = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6" # ModeEffectClsid

$old5 = (Get-ItemProperty $fx -ErrorAction SilentlyContinue).$k5
$old6 = (Get-ItemProperty $fx -ErrorAction SilentlyContinue).$k6

"old5=$old5"
"old6=$old6"
```

### 2.3 Set StreamEffectClsid and ModeEffectClsid to the Equalizer CLSID
These keys are often ACL-protected. If normal admin PowerShell fails, run this from a **SYSTEM** shell.

#### 2.3.1 Open a SYSTEM PowerShell
If you already have `PsExec.exe` on disk, run (PowerShell admin):

```powershell
C:\path\to\PsExec.exe -accepteula -i -s powershell.exe
```

If you **do not** have it yet, download PSTools first (PowerShell admin):

```powershell
$u  = "https://download.sysinternals.com/files/PSTools.zip"
$zip = "$env:TEMP\PSTools.zip"
$dst = "$env:TEMP\PSTools"
Invoke-WebRequest -Uri $u -OutFile $zip
Expand-Archive -Path $zip -DestinationPath $dst -Force
& "$dst\PsExec.exe" -accepteula -i -s powershell.exe
```

Then in the new SYSTEM window, confirm:

```powershell
whoami
```

Expected: `nt authority\\system`

#### 2.3.2 Apply the endpoint FX values
In that SYSTEM window (replace endpoint GUID):

```powershell
$clsid = "{8E259F55-B32B-4FB8-8995-5965798B2C08}"
$ep = "{312a1cfb-cb49-4bbe-a050-6745dd210145}"
$fx = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$ep\FxProperties"

Set-ItemProperty -Path $fx -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -Type String -Value $clsid
Set-ItemProperty -Path $fx -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6" -Type String -Value $clsid
```

If PowerShell still reports access denied, try `reg.exe` instead (SYSTEM window):

```powershell
$clsid = "{8E259F55-B32B-4FB8-8995-5965798B2C08}"
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{312a1cfb-cb49-4bbe-a050-6745dd210145}\FxProperties" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" /t REG_SZ /d $clsid /f
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{312a1cfb-cb49-4bbe-a050-6745dd210145}\FxProperties" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6" /t REG_SZ /d $clsid /f
```

### 2.4 Restart audio engine
Run in **elevated CMD**:

```cmd
net stop audiosrv
net start audiosrv
```

---

## 3) Verify it is actually loaded/active

### 3.1 Check audiodg and loaded modules
Run in **PowerShell (admin)**:

```powershell
Get-Process audiodg -ErrorAction SilentlyContinue | Select Id, ProcessName
(Get-Process audiodg -ErrorAction SilentlyContinue).Modules |
  Where-Object { $_.ModuleName -ieq 'Equalizer.dll' } |
  Select FileName
```
### 3.2 Check file-based trace (if built with tracing)
If your build includes the file-based trace, it will write:
- `C:\driver\eq_apo.txt`

Check it:

```powershell
Get-Content "C:\driver\eq_apo.txt" -ErrorAction SilentlyContinue
```

---

## 4) Disable (restore previous settings)

Run in the same privilege level you used to enable it (admin or SYSTEM).

Replace endpoint GUID:

```powershell
$ep = "{312a1cfb-cb49-4bbe-a050-6745dd210145}"
$fx = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$ep\FxProperties"
$k5 = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"
$k6 = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6"

# If you saved old5/old6:
if ($old5) { Set-ItemProperty -Path $fx -Name $k5 -Type String -Value $old5 } else { Remove-ItemProperty -Path $fx -Name $k5 -ErrorAction SilentlyContinue }
if ($old6) { Set-ItemProperty -Path $fx -Name $k6 -Type String -Value $old6 } else { Remove-ItemProperty -Path $fx -Name $k6 -ErrorAction SilentlyContinue }
```

Restart audio:

```cmd
net stop audiosrv
net start audiosrv
```

---

## 5) Uninstall (clean removal)

### 5.1 Unregister
Run in **elevated CMD**:

```cmd
C:\Windows\System32\regsvr32.exe /u "C:\driver\Equalizer.dll"
```

### 5.2 Restore registry backups (optional, safest)
Run in **elevated CMD**:

```cmd
reg import "C:\driver\mmdevices_render_backup.reg"
reg import "C:\driver\apo_catalog_backup.reg"
```

Restart audio:

```cmd
net stop audiosrv
net start audiosrv
```

### 5.3 Delete the DLL (optional)
```cmd
del "C:\driver\Equalizer.dll"
```

---

## Other installation option: INF-based (test-only)

If direct edits to `...\\MMDevices\\Audio\\Render\\{...}\\FxProperties` are blocked (access denied), you can try installing via the test INF in this repo: `installer/EqualizerTest.inf`.

### A) Prepare the INF
1. Copy your built `Equalizer.dll` into the same directory as the INF (e.g. `C:\driver\eqinf`).
2. Open `EqualizerTest.inf` and replace:
   - `{REPLACE-WITH-ENDPOINT-GUID}` with your target render endpoint GUID.

### B) Install
Run in **elevated CMD**:

```cmd
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 C:\driver\eqinf\EqualizerTest.inf
```

### C) Uninstall
Run in **elevated CMD**:

```cmd
rundll32.exe setupapi.dll,InstallHinfSection DefaultUninstall 132 C:\driver\eqinf\EqualizerTest.inf
```

### D) Logs if the install fails
Check:
- `C:\Windows\INF\setupapi.dev.log`

## Other installation option: Realtek device extension INF (test-only)

If you want the package to be device-associated (better chance on Win11), use:
- `installer/EqualizerRealtekExtension.inf`

### A) Prepare
1. Copy your built `Equalizer.dll` next to the INF (e.g. `C:\driver\eqext`).
2. Copy `installer/EqualizerRealtekExtension.inf` into the same folder.
3. Edit the INF and replace:
   - `{REPLACE-WITH-ENDPOINT-GUID}` with your Realtek render endpoint GUID.

### B) Install (device-associated)
Run in **elevated CMD**:

```cmd
pnputil /add-driver C:\driver\eqext\EqualizerRealtekExtension.inf /install
```

### C) Uninstall
List installed driver packages and remove the one that corresponds to your INF:

```cmd
pnputil /enum-drivers
```

Then:

```cmd
pnputil /delete-driver oemXX.inf /uninstall /force
```

(Replace `oemXX.inf` with the published name printed by `/enum-drivers`.)

Optional manual cleanup using the INF section:

```cmd
rundll32.exe setupapi.dll,InstallHinfSection DefaultUninstall 132 C:\driver\eqext\EqualizerRealtekExtension.inf
