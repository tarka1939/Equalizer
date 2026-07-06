# Raport Techniczny - Equalizer APO
## System equalizera audio dla Windows

---

## 1. Opis techniczny środowiska developerskiego

### 1.1 Platforma docelowa
- **System operacyjny**: Windows 10/11 (64-bit)
- **Architektura**: x64
- **Środowisko programistyczne**: Microsoft Visual Studio
- **Język programowania**: C++ (standard C++14/17)

### 1.2 Technologie i frameworki
- **Windows Audio Session API (WASAPI)**
- **Audio Processing Objects (APO) API**
- **Component Object Model (COM)**
- **Windows Runtime C++ Template Library (WRL)**

### 1.3 Wymagane biblioteki systemowe
- `audioenginebaseapo.h` - Bazowa klasa dla APO
- `audiopolicy.h` - Polityki audio Windows
- `mmdeviceapi.h` - API do zarządzania urządzeniami multimedialnymi
- `propkey.h` - Klucze właściwości systemowych
- `wrl.h` - Windows Runtime Library

### 1.4 Kompilacja
Projekt jest skonfigurowany jako:
- **Typ**: Dynamic Link Library (DLL)
- **Konfiguracje**: Debug/Release
- **Platforma**: x64
- **Wyjściowy plik**: `Equalizer.dll`

---

## 2. Opis zastosowanych rozwiązań technicznych

### 2.1 Audio Processing Object (APO)
APO to komponent Windows Audio Engine, który przetwarza strumienie audio w czasie rzeczywistym. Projekt implementuje niestandardowy APO jako obiekt COM.

**Kluczowe interfejsy COM:**
- `IAudioProcessingObject` - Podstawowy interfejs APO
- `IAudioProcessingObjectRT` - Interfejs do przetwarzania w czasie rzeczywistym
- `IAudioProcessingObjectConfiguration` - Konfiguracja APO

**CLSID APO:**
```cpp
{12345678-9ABC-4DEF-8011-223344556677}
```

### 2.2 Architektura przetwarzania cyfrowego sygnału (DSP)

#### Filtry Biquad
Podstawowa jednostka przetwarzania to **filtr biquadowy** (IIR drugiego rzędu):
- Implementacja typu "peaking EQ" (wzmocnienie/tłumienie pasma)
- Współczynniki filtru: b0, b1, b2, a1, a2
- Podwójne buforowanie współczynników dla bezpiecznej zmiany w czasie rzeczywistym
- Atomowe przełączanie między zestawami współczynników

#### Equalizer 10-pasmowy
- **10 niezależnych pasm częstotliwości**
- Każde pasmo to filtr biquad peaking
- Kaskadowe połączenie filtrów
- Wspólny współczynnik Q dla wszystkich pasm

#### RT-safe design
- Przetwarzanie bez alokacji pamięci
- Bez blokad (lock-free)
- Atomowe operacje dla współdzielonych danych
- Oddzielenie ścieżki czasu rzeczywistego (RT) od konfiguracji (non-RT)

### 2.3 Rejestracja COM i APO

#### Rejestracja COM
Standardowa rejestracja in-process server:
```
HKLM\SOFTWARE\Classes\CLSID\{CLSID}\InprocServer32
```
- Ścieżka do DLL
- ThreadingModel = Both

#### Rejestracja w katalogu APO
```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\{CLSID}
```
Właściwości:
- FriendlyName
- MajorVersion/MinorVersion
- Flags
- MinInputConnections/MaxInputConnections

#### Aktywacja w endpoint
```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{EndpointGUID}\FxProperties
```
Kluczowe właściwości:
- `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5` - StreamEffectClsid
- `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6` - ModeEffectClsid

### 2.4 Diagnostyka i debugowanie
- **OutputDebugString**: Logi dla debugera Visual Studio
- **Logi plikowe**: `C:\driver\eq_apo.txt` dla weryfikacji aktywacji
- Śledzenie cyklu życia DLL (DLL_PROCESS_ATTACH/DETACH)

---

## 3. Opis architektury projektu

### 3.1 Struktura katalogów

```
Equalizer/
├── Equalizer/          # Główny moduł APO
│   ├── Equalizer.cpp/h       # Klasa główna APO
│   ├── BandEqualizer.cpp/h   # Zarządzanie pasmami
│   ├── ComExports.cpp        # Eksporty COM (DllGetClassObject, DllRegisterServer)
│   ├── dllmain.cpp          # Entry point DLL
│   ├── Diagnostics.h        # Narzędzia diagnostyczne
│   └── Equalizer.def        # Plik definicji eksportów
│
├── DSP/                # Moduł przetwarzania sygnału
│   ├── Biquad.cpp/h         # Implementacja filtru biquad
│   ├── Equalizer10Band.cpp/h # Equalizer 10-pasmowy
│   └── Equalizer10Band.inline.h
│
├── installer/          # Pliki instalacyjne
│   ├── EqualizerRealtekExtension.inf  # INF dla Realtek
│   └── EqualizerTest.inf              # INF testowy
│
└── Tools/              # Narzędzia testowe
    └── WavEqTest.cpp   # Test equalizera na plikach WAV
```

### 3.2 Diagram klas

```
┌─────────────────────────────────────┐
│         Equalizer (APO)             │
│  ┌────────────────────────────┐    │
│  │ IAudioProcessingObject     │    │
│  │ IAudioProcessingObjectRT   │    │
│  │ IAudioProcessingObjectConfiguration │
│  └────────────────────────────┘    │
│                                     │
│  + APOProcess()                    │
│  + GetRegistrationProperties()     │
│  + LockForProcess()                │
└──────────────┬──────────────────────┘
               │ używa
               ▼
┌──────────────────────────────────────┐
│    DSP::Equalizer10Band             │
│                                      │
│  - m_bands: array<Biquad, 10>       │
│  + Process()                         │
│  + SetBandsPeaking()                 │
│  + Prepare()                         │
└──────────────┬───────────────────────┘
               │ składa się z
               ▼
┌──────────────────────────────────────┐
│        DSP::Biquad                  │
│                                      │
│  - m_coeffs[2]: Coeffs              │
│  - m_activeIndex: atomic<uint32>    │
│  - m_states: vector<ChannelState>   │
│  + Process()                         │
│  + SetPeaking()                      │
│  + SetCoefficients()                 │
└──────────────────────────────────────┘
```

### 3.3 Przepływ danych audio

```
Aplikacja audio (np. Spotify, Chrome)
         ↓
Windows Audio Engine (audiodg.exe)
         ↓
    APO Pipeline
         ↓
Equalizer::APOProcess()
         ↓
    [Preamp gain]
         ↓
DSP::Equalizer10Band::Process()
         ↓
    [10 filtrów Biquad kaskadowo]
         ↓
    [Safety clamp ±1.0f]
         ↓
    Wyjście audio
         ↓
Sterownik urządzenia audio (Realtek/inne)
         ↓
    Głośniki/Słuchawki
```

---

## 4. Opis działania programu

### 4.1 Inicjalizacja APO

1. **Ładowanie DLL**: Windows Audio Engine (`audiodg.exe`) ładuje `Equalizer.dll`
2. **Tworzenie instancji**: COM tworzy obiekt `Equalizer` przez `DllGetClassObject`
3. **Konfiguracja**: Wywoływane są metody `Initialize()` i `LockForProcess()`
4. **Przygotowanie DSP**: Equalizer 10-pasmowy jest inicjalizowany z częstotliwością próbkowania i liczbą kanałów

### 4.2 Przetwarzanie w czasie rzeczywistym

Metoda `Equalizer::APOProcess()` jest wywoływana przez audio engine dla każdego bufora audio:

1. **Walidacja**: Sprawdzenie połączeń wejściowych/wyjściowych
2. **Preamp**: Aplikacja wzmocnienia wstępnego (gain)
   ```cpp
   out[i] = in[i] * m_gain;
   ```
3. **Equalizacja**: Przetwarzanie przez 10 pasm
   ```cpp
   s_eq.Process(out, out, frameCount, channels);
   ```
4. **Clipping protection**: Ograniczenie do zakresu [-1.0, 1.0]
   ```cpp
   if (out[i] > 1.0f) out[i] = 1.0f;
   else if (out[i] < -1.0f) out[i] = -1.0f;
   ```

### 4.3 Algorytm filtra biquad

Dla każdej próbki audio:
```
y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
```

Gdzie:
- x[n] = próbka wejściowa
- y[n] = próbka wyjściowa
- b0, b1, b2, a1, a2 = współczynniki filtru

### 4.4 Obliczanie współczynników peaking EQ

```cpp
omega = 2 * PI * centerHz / sampleRate
alpha = sin(omega) / (2 * Q)
A = 10^(gainDb / 40)

b0 = 1 + alpha * A
b1 = -2 * cos(omega)
b2 = 1 - alpha * A
a0 = 1 + alpha / A
a1 = -2 * cos(omega)
a2 = 1 - alpha / A
```

### 4.5 Domyślne pasma częstotliwości

Equalizer 10-pasmowy działa na następujących częstotliwościach środkowych (standardowe pasma):
- 31 Hz
- 62 Hz
- 125 Hz
- 250 Hz
- 500 Hz
- 1 kHz
- 2 kHz
- 4 kHz
- 8 kHz
- 16 kHz

---

## 5. Przykładowe uruchomienie i kompilacja

### 5.1 Kompilacja projektu

#### W Visual Studio:
1. Otwórz projekt w Visual Studio
2. Wybierz konfigurację: **Release**
3. Wybierz platformę: **x64**
4. Build → Build Solution (Ctrl+Shift+B)
5. Plik wyjściowy: `x64\Release\Equalizer.dll`

#### Z wiersza poleceń (MSBuild):
```cmd
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Equalizer.sln /p:Configuration=Release /p:Platform=x64
```

### 5.2 Instalacja APO

#### Krok 1: Kopiowanie DLL
```cmd
mkdir C:\driver
copy x64\Release\Equalizer.dll C:\driver\
```

#### Krok 2: Rejestracja COM (jako Administrator)
```cmd
C:\Windows\System32\regsvr32.exe "C:\driver\Equalizer.dll"
```

Powinno się pojawić: "DllRegisterServer in C:\driver\Equalizer.dll succeeded."

#### Krok 3: Weryfikacja rejestracji COM
```powershell
$clsid = "{12345678-9ABC-4DEF-8011-223344556677}"
Get-ItemProperty "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"
```

#### Krok 4: Weryfikacja katalogu APO
```powershell
Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\$clsid"
```

### 5.3 Aktywacja na endpoint audio

#### Lista dostępnych urządzeń:
```powershell
$renderBase = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
Get-ChildItem $renderBase | ForEach-Object {
  $props = "Registry::$($_.Name)\Properties"
  $name = (Get-ItemProperty -Path $props -ErrorAction SilentlyContinue)."{b3f8fa53-0004-438e-9003-51a46e139bfc},6"
  [PSCustomObject]@{ RenderGuid = $_.PSChildName; FriendlyName = $name }
}
```

#### Aktywacja (wymaga uprawnień SYSTEM):
```powershell
# Uruchom PowerShell jako SYSTEM
# psexec -i -s powershell.exe

$ep = "{GUID-TWOJEGO-ENDPOINT}"
$fx = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$ep\FxProperties"
$clsid = "{12345678-9ABC-4DEF-8011-223344556677}"

Set-ItemProperty $fx -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -Value $clsid -Type String
Set-ItemProperty $fx -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6" -Value $clsid -Type String
```

#### Restart audio engine:
```cmd
net stop audiosrv
net start audiosrv
```

### 5.4 Odinstalowanie

#### Wyrejestrowanie COM:
```cmd
regsvr32 /u "C:\driver\Equalizer.dll"
```

#### Przywrócenie oryginalnych ustawień endpoint:
```cmd
reg import C:\driver\mmdevices_render_backup.reg
```

---

## 6. Testy

### 6.1 Test jednostkowy filtra Biquad

**Lokalizacja**: `Tools\WavEqTest.cpp`

**Cel**: Test przetwarzania equalizera na plikach WAV

**Funkcjonalność**:
- Wczytywanie pliku WAV (32-bit float)
- Konfiguracja equalizera 10-pasmowego
- Przetwarzanie bufora audio
- Zapis wynikowego pliku WAV

**Przykładowe użycie**:
```cpp
// Wczytaj plik WAV
FmtChunk fmt;
std::vector<float> samples;
if (!ReadWavFloat32("input.wav", fmt, samples)) {
    printf("Error reading WAV\n");
    return 1;
}

// Skonfiguruj equalizer
DSP::Equalizer10Band eq;
eq.Prepare(fmt.sampleRate, fmt.numChannels);

// Ustaw wzmocnienie dla pasm
std::array<float, 10> centerHz = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
std::array<float, 10> gainDb = {6, 3, 0, -3, 0, 0, 3, 6, 3, 0}; // Przykładowa krzywa V
eq.SetBandsPeaking(centerHz, gainDb, 1.0f);

// Przetwórz
const uint32_t frames = samples.size() / fmt.numChannels;
eq.Process(samples.data(), samples.data(), frames, fmt.numChannels);

// Zapisz
WriteWavFloat32("output.wav", fmt, samples);
```

### 6.2 Test weryfikacji ładowania APO

**Cel**: Sprawdzenie, czy Windows Audio Engine ładuje APO

**Metoda 1: Process Monitor**
```
1. Uruchom Process Monitor (Sysinternals)
2. Filtr: Process Name is audiodg.exe
3. Filtr: Path contains Equalizer.dll
4. Rozpocznij odtwarzanie audio
5. Sprawdź, czy DLL jest ładowana
```

**Metoda 2: Logi plikowe**
Dodaj do `DllMain`:
```cpp
case DLL_PROCESS_ATTACH:
    CreateFileW(L"C:\\driver\\eq_apo_loaded.txt", GENERIC_WRITE, ...);
    break;
```

**Metoda 3: ListDLLs (Sysinternals)**
```cmd
listdlls audiodg.exe | findstr /i equalizer
```

### 6.3 Test poprawności przetwarzania sygnału

**Sygnał testowy sine wave**:
```cpp
// Generuj tonację 1kHz
const float freq = 1000.0f;
const float sampleRate = 48000.0f;
const uint32_t duration = 1; // sekunda
const uint32_t samples = sampleRate * duration;

std::vector<float> sine(samples);
for (uint32_t i = 0; i < samples; ++i) {
    sine[i] = std::sin(2.0f * M_PI * freq * i / sampleRate);
}

// Zastosuj equalizer z boostem na 1kHz
DSP::Equalizer10Band eq;
eq.Prepare(sampleRate, 1);
std::array<float, 10> gains = {0, 0, 0, 0, 0, 6, 0, 0, 0, 0}; // +6dB @ 1kHz
eq.SetBandsPeaking(centerHz, gains, 1.0f);
eq.Process(sine.data(), sine.data(), samples, 1);

// Weryfikuj wzmocnienie: amplituda powinna wzrosnąć ~2x dla +6dB
```

### 6.4 Test wydajności RT-safety

**Pomiar czasu przetwarzania**:
```cpp
#include <chrono>

auto start = std::chrono::high_resolution_clock::now();
eq.Process(buffer, buffer, frames, channels);
auto end = std::chrono::high_resolution_clock::now();

auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
float samplesPerSecond = (frames * channels) / (duration.count() / 1e6f);

// Dla RT: samplesPerSecond powinno być >> sampleRate
printf("Throughput: %.2f Msps (%.2fx realtime)\n", 
       samplesPerSecond / 1e6f, 
       samplesPerSecond / sampleRate);
```

---

## 7. Opis problemów napotkanych

### 7.1 Problem: APO nie jest ładowane przez Audio Engine

**Objawy**:
- Rejestracja COM i APO katalog wykonane poprawnie
- Endpoint FxProperties ustawione na CLSID equalizera
- Plik diagnostyczny `C:\driver\eq_apo.txt` nie jest tworzony
- `audiodg.exe` nie ładuje `Equalizer.dll`

**Przyczyna**:
Na Windows 11 z OEM audio stack (Realtek UAPO + Nahimic APO), standardowe podejście z ustawieniem `StreamEffectClsid`/`ModeEffectClsid` nie jest wystarczające. OEM driver używa bardziej złożonego mechanizmu chain effects z dodatkowymi właściwościami:
```
{d3993a3f-99c2-4402-b5ec-a92a0367664b},5/6/7/11/12 (MultiString lists)
{670173E3-78CF-11E5-A837-0800200C9A66} (mode-related GUID groups)
```

### 7.2 Problem: Access Denied przy zapisie do rejestru endpoint

**Objawy**:
```
Set-ItemProperty : Requested registry access is not allowed.
```

**Przyczyna**:
Klucze w `HKLM\...\MMDevices\Audio\Render\{GUID}\FxProperties` są chronione przez:
- Zaawansowane ACL (Access Control Lists)
- Security descriptors zdefiniowane przez driver
- Nawet konto SYSTEM ma ograniczony dostęp

**Obejścia testowane**:
1. ✗ Uruchomienie PowerShell jako Administrator - niewystarczające
2. ✗ Uruchomienie jako SYSTEM (PsExec) - nadal blokowane
3. ✓ Instalacja przez INF z `InstallHinfSection` - działa częściowo
4. ✓ Extension INF dla konkretnego urządzenia - wymaga signed driver

**Rozwiązanie**:
Użycie pliku INF z sekcją `AddReg` i instalacja przez:
```cmd
rundll32 setupapi.dll,InstallHinfSection DefaultInstall 132 .\installer\EqualizerTest.inf
```

### 7.3 Problem: Blokada Secure Boot i VBS

**Objawy**:
- Instalacja extension INF odrzucana przez PnP
- "INF is not a valid extension driver package"

**Przyczyna**:
- **Secure Boot** wymaga podpisu cyfrowego driver (catalog .cat)
- **Virtualization-Based Security** (VBS/Device Guard) blokuje niepodpisane komponenty kernel-mode

**Rozwiązanie tymczasowe**:
1. Wyłącz Secure Boot w BIOS/UEFI
2. Wyłącz VBS:
   ```cmd
   bcdedit /set hypervisorlaunchtype off
   reg add "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard" /v EnableVirtualizationBasedSecurity /t REG_DWORD /d 0 /f
   ```
3. Restart systemu

**Rozwiązanie produkcyjne**:
- Uzyskanie certyfikatu code signing (Extended Validation)
- Stworzenie catalog file (.cat) przez `inf2cat`
- Podpisanie driver przez `signtool`
- Przesłanie do Microsoft Hardware Dev Center for attestation signing

### 7.4 Problem: Konflikty z istniejącymi OEM APO

**Objawy**:
- Realtek UAPO (`RltkAPOU642.dll`) i Nahimic APO (`NahimicAPO4.dll`) są już załadowane
- Niestandardowy equalizer nie jest dodawany do pipeline

**Przyczyna**:
OEM driver tworzy zamkniętą chain effects. Nadpisanie `StreamEffectClsid` całkowicie zastępuje OEM chain, co powoduje utratę funkcjonalności producenta (np. wirtualny surround, redukcja szumów).


### 7.5 Problem: Brak dokumentacji Audio Effects Discovery

**Objawy**:
- INF Extension jest akceptowany, ale APO nie jest aktywowany
- Brak jasnych przykładów integracji z Realtek UAPO

**Przyczyna**:
Microsoft wprowadziło nowy model **Audio Effects Discovery** w Windows 11, ale dokumentacja jest fragmentaryczna. Model wymaga:
- Property store keys dla effects capabilities
- Deklaracja effect types (EQ, Reverb, etc.)
- Integration z Settings app

### 7.6 Problem: Threading model APO

**Objawy**:
- Sporadyczne crackling audio
- Race conditions przy zmianie parametrów

**Przyczyna**:
`APOProcess()` jest wywoływane w wątku wysokiego priorytetu (RT thread). Każda blokada lub alokacja pamięci powoduje przeoczenia buffer (underrun).

---

## Podsumowanie

Projekt implementuje funkcjonalny 10-pasmowy equalizer jako Windows Audio Processing Object z następującymi osiągnięciami:

✅ **Kompletna implementacja DSP**: Filtry biquad RT-safe z konfigurowalnymi współczynnikami  
✅ **Pełna integracja COM/APO**: Wszystkie wymagane interfejsy WASAPI  
✅ **Działający kod**: Equalizer przetwarza audio poprawnie w środowiskach testowych  
✅ **Narzędzia testowe**: WavEqTest dla offline testing   

⚠️ **Główne wyzwanie**: Integracja z OEM audio stacks (Realtek UAPO) na Windows 11 wymaga extension driver z podpisem cyfrowym.

---

**Autor**: Krzysztof Tarka  
**Data**: 29 Styczeń 2026  
**Wersja**: 1.0.0  
