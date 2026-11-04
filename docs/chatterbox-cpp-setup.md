# Chatterbox.cpp native voice backend

This document records the durable local layout and the validated native voice path for ARLO Mini. It is intentionally free of credentials and machine-specific secrets.

## Repository boundaries

`chatterbox.cpp` is a separate Git repository. The ARLO Mini bridge launches its executable and does not vendor the native source into this repository.

The active Windows layout is:

```text
C:\Users\gh0st\Documents\chatterbox.cpp\
  build-cuda\Release\tts-cli.exe
  build-cuda\bin\Release\              # CUDA and ggml DLLs
  models\chatterbox-t3-turbo-f16.gguf
  models\chatterbox-s3gen-f16.gguf

C:\Users\gh0st\Documents\arlo-mini\
  voice\miss-minutes-profile\           # local, ignored five-tensor profile
```

Do not place the executable, GGUF models, or voice profile in the Hermes scratch directory. Scratch files are temporary and may be pruned.

The following local files are intentionally ignored and must be recreated or backed up separately on another machine:

- `bridge/.env`
- `firmware/include/secrets.h`
- `voice/miss-minutes-profile/`

Never commit credentials, Wi-Fi settings, device keys, Hermes API keys, or private voice recordings.

## Current validated configuration

The raw PCM path requires both settings:

```env
TTS_BACKEND=chatterbox_cpp
CHATTERBOX_CPP_BATCH=false
CHATTERBOX_CPP_LIVE_STREAM=true
```

The remaining native settings point to the separate checkout:

```env
CHATTERBOX_CPP_BINARY=C:/Users/gh0st/Documents/chatterbox.cpp/build-cuda/Release/tts-cli.exe
CHATTERBOX_CPP_MODEL=C:/Users/gh0st/Documents/chatterbox.cpp/models/chatterbox-t3-turbo-f16.gguf
CHATTERBOX_CPP_S3GEN=C:/Users/gh0st/Documents/chatterbox.cpp/models/chatterbox-s3gen-f16.gguf
CHATTERBOX_CPP_PROFILE=C:/Users/gh0st/Documents/arlo-mini/voice/miss-minutes-profile
CHATTERBOX_CPP_RUNTIME_PATHS=C:/Users/gh0st/Documents/chatterbox.cpp/build-cuda/bin/Release;C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v11.6/bin
CHATTERBOX_CPP_THREADS=4
CHATTERBOX_CPP_GPU_LAYERS=99
```

The K10 firmware must use the direct live sink:

```cpp
constexpr bool PCM_DIAGNOSTIC_USE_SDK_PLAYBACK = false;
```

The bridge normalizes native **24 kHz mono S16LE** to **16 kHz stereo S16LE** before exposing `/jobs/<id>/pcm`. The K10 buffers the stream in PSRAM and sends it to `i2s_write()` while synthesis continues.

## Windows raw PCM requirement

Windows stdout is text-mode by default. The native CLI must switch stdout to binary mode before emitting raw PCM:

```cpp
#ifdef _WIN32
_setmode(_fileno(stdout), _O_BINARY);
#endif
```

Without this, newline bytes in the PCM stream can be expanded to CRLF. The bridge and K10 may still report successful byte counts, but sample alignment is corrupted and the speaker produces static.

The fix belongs in the separate `chatterbox.cpp` checkout, in `src/chatterbox_cli.cpp`, and must be rebuilt whenever that checkout is recreated:

```powershell
cd C:\Users\gh0st\Documents\chatterbox.cpp
cmake --build build-cuda --config Release --target tts-cli -j 8
```

Keep the source change committed in the `chatterbox.cpp` repository separately from the ARLO Mini firmware/bridge commits.

## Run the bridge

From the ARLO Mini repository:

```powershell
.\.chatterbox-venv\Scripts\python.exe bridge/server.py
```

Verify the process is using the configured native executable and that the listener is present. A health response of HTTP 401 without the device key still proves the listener is active and authentication is enforced.

## Build and flash the K10

From the ARLO Mini repository:

```powershell
.venv\Scripts\python.exe -m platformio run --project-dir firmware
.venv\Scripts\python.exe -m platformio run --project-dir firmware --target upload --upload-port COM8
```

The project uses `upload_speed = 115200` because higher-speed uploads have previously lost the K10 USB connection. After flashing, capture a separate boot and require `ASR READY` and `SETUP COMPLETED`; a successful upload alone is not physical validation.

## Validation checklist

A real raw PCM test should show all of these on the K10:

```text
PCM stream download bytes=<nonzero> failed=0
PCM playback start buffered=<nonzero> downloaded=<nonzero>
PCM I2S playback clock requested=16000 actual=16000 result=0
PCM playback stats ... empty_reads=0 ... partial=0 ... requested=<N> written=<N>
PCM stream playback complete bytes=<N>
```

The final acceptance check is physical: the K10 must produce intelligible speech, not only successful host-side statistics. The known-good direct-I2S diagnostic tone was temporary and has been removed from the production firmware.

## Rollback

To return to the complete-WAV fallback without deleting the native setup:

```env
CHATTERBOX_CPP_LIVE_STREAM=false
```

That keeps `TTS_BACKEND=chatterbox_cpp` but uses complete native output and the K10 SD/WAV playback path instead of raw PCM streaming.
