# ARLO Mini - hands-free UNIHIKER K10 voice companion

**Status: working prototype validated on a physical UNIHIKER K10.** This repository contains the K10 firmware, a Python voice bridge, offline protocol tests, and sanitized configuration templates. Local credentials, Wi-Fi settings, generated audio, build output, and Hermes runtime metadata are intentionally excluded.

## What it does

1. K10 listens locally for the **factory English wake word “Jarvis” or “Hi Telly”**. **It does not yet respond to “Hey ARLO.”**
2. The K10 records a fixed **5-second WAV** through its onboard microphone into a FAT32 microSD card.
3. Over your private 2.4 GHz Wi-Fi, it uploads the WAV to a Python bridge on your computer.
4. `faster-whisper` transcribes locally; the bridge calls your **Hermes Agent gateway** at `127.0.0.1:8642`.
5. The bridge synthesizes speech locally (Windows built-in speech, Linux `espeak-ng`, or Piper) and converts it to **16kHz / 16-bit / stereo PCM WAV** with FFmpeg.
6. The K10 downloads that file, speaks through its speaker, and animates its eyes. It then offers **two automatic 5-second follow-up windows** without needing another wake word. Silence exits.

This is **not** full-duplex or interruption-ready. Audio is recorded in fixed windows; speech detection is performed on the server, not in real time. It is an inexpensive first milestone, not yet the final conversational behavior.

## Prerequisites

- UNIHIKER **K10** with working English voice model. Firmware flashes replace the current ESPClaw sketch; back up any existing firmware/configuration first.
- FAT32 microSD (TF) card, preferably <=32GB.
- Your PC and K10 on the same 2.4GHz-capable private Wi-Fi network, with Windows Firewall/LAN rules permitting inbound TCP 8765 **from the K10 only**.
- Hermes Agent configured and running on the **same PC as the bridge** (local default API `http://127.0.0.1:8642`).
- Python **3.11** suggested. `ffmpeg` available on PATH. Windows has built-in SAPI voice; Linux needs `espeak-ng`, or use Piper.
- PlatformIO VS Code extension or CLI. First install requires internet because PlatformIO fetches DFRobot's platform and firmware dependencies. First STT startup also downloads a Whisper model if not already cached.

## Install the Python bridge

From `bridge/` on the PC:

**Windows PowerShell:**

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
Copy-Item .env.example .env
notepad .env
python server.py
```

**Linux:**

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
nano .env
python server.py
```

Configure `bridge/.env`:

- `DEVICE_KEY`: set a unique long secret (the same one as firmware `secrets.h`).
- `HERMES_KEY`: the value you use for `API_SERVER_KEY` in `~/.hermes/.env`.
- `HERMES_URL`: keep `http://127.0.0.1:8642/v1/chat/completions` if both run on the PC.
- `STT_MODEL`: `base.en` is a modest starting point, change to `small.en` for greater accuracy (slower on CPU).
- `TTS_BACKEND`: `auto`, `piper`, `sapi`, or `espeak`. For `piper`, set `PIPER_MODEL` and `PIPER_BINARY`.
- To test without a running Hermes, set `MOCK_HERMES=true` (it echoes the transcription).

In `~/.hermes/.env` ensure:

```env
API_SERVER_ENABLED=true
API_SERVER_KEY=your-hermes-api-key
# Keep the server loopback-only; bridge talks to it locally.
API_SERVER_HOST=127.0.0.1
```

Run `hermes gateway` in another terminal. Do **not** bind Hermes itself to `0.0.0.0` for this prototype. Refer to its [API server documentation](https://hermes-agent.nousresearch.com/docs/user-guide/features/api-server/).

**Connectivity test on the PC:**

```powershell
curl.exe -H "X-Device-Key: YOUR_DEVICE_KEY" http://127.0.0.1:8765/health
```

Expected: `{"ok":true,"name":"ARLO Mini bridge",...}`. Then run same request from another device using the PC's LAN IP in place of 127.0.0.1.

## Install K10 firmware

1. Open `firmware/` as a PlatformIO project in VS Code.
2. Copy `firmware/include/secrets.example.h` to `firmware/include/secrets.h`.
3. Set your 2.4GHz Wi-Fi credentials. Set `BRIDGE_BASE_URL` to `http://YOUR_PC_LAN_IP:8765` (not `localhost`), plus `DEVICE_KEY` exactly as in the bridge.
4. Insert a FAT32 microSD card before powering K10.
5. Plug the K10 into USB-C; PlatformIO **Build**, then **Upload**. Use **Monitor** at 115200 baud for debug logs.
6. Speak **“Jarvis”**, pause until its eyes show "Listening now", and speak your question **within the 5-second recording window**. Wait for speech playback. Then simply speak a follow-up during the next recording window.

> **Language model warning:** If the board currently has the Chinese speech model flashed, `EN_MODE` may not recognize English wake words. Use DFRobot's [K10 documentation](https://www.unihiker.com/wiki/K10/) to restore the English recognition model, if needed.

> **SD warning:** The SDK's `initSDFile()` loops indefinitely when the card is not mounted. This firmware checks `SD.begin()` first and displays an error if it's missing. The framework still needs to be tested on your actual K10.

## Test the backend without hardware

```bash
python -m pip install pytest httpx
python -m pytest -q tests/
```

The tests mock transcription and synthesis, verify authentication, reject invalid/oversized input, exercise the job queue and fetch generated WAV bytes. They do **not** prove firmware compilation, speaker playback, or Wi-Fi functionality.

## Troubleshooting

| Problem | Check |
|---|---|
| Nothing happens on “Jarvis” | Confirm `EN_MODE` voice model; inspect serial logs; try "Hi Telly" |
| Wi-Fi disconnected | K10 uses 2.4GHz Wi-Fi, verify SSID/password and LAN reachability |
| HTTP upload failed | Correct PC LAN IP, firewall allowing **TCP 8765** from K10, matching `DEVICE_KEY` |
| Bridge says 401 | `DEVICE_KEY` mismatch; Hermes 401 means wrong `HERMES_KEY` |
| Bridge says 500 / job error | Check bridge console and `/jobs/<id>` `error` field |
| Talking is slow | First-use Whisper model download, Hermes latency, local CPU vs GPU inference |
| No or garbled audio | Confirm K10 SDK WAV playback in official recording/playback example, `ffmpeg` installed, try different sample rate if your firmware version differs |
| Voice doesn't speak | `powershell.exe` SAPI or `espeak-ng` missing; switch to Piper |
| Hermes responds but cannot see files | Set Hermes tool permissions and working directory as appropriate; voice requests do not inherently grant file access |
| Fails building on PC | Verify official `platform-unihiker` install and the exact K10 board/core version; SDK changed APIs may need adapting |

## Privacy and limitations

- `DEVICE_KEY` authenticates requests on a **trusted LAN**, but HTTP has **no encryption**. Don't use public Wi-Fi, expose port 8765 to the internet, or forward that port. Use Tailscale/VPN or TLS termination if you need remote access later.
- Wake-word detection is performed locally by the factory speech recognizer; once awake, the **entire fixed 5-second recording is sent over local Wi-Fi**. Follow-up windows also record. The bridge uses temporary files cleaned after the job expires (20 minutes), not a permanent recording archive.
- Hermes tools can affect your computer. For a voice-driven agent, restrict shell/filesystem/destructive tool permissions and require approvals for risky actions.
- Fixed-duration capture can clip speech and has no barge-in. Some environmental speech can still be captured in automatic follow-up windows.
- The custom **“Hey ARLO”** wake word, true continuous conversational VAD, and hardware compile/real-device playback validation are future work.

## Key sources

- [DFRobot official K10 Arduino examples (ASR, SD recording, canvas)](https://www.unihiker.com/wiki/K10/Examples/examples_arduinoide/)
- [DFRobot PlatformIO K10 platform](https://github.com/DFRobot/platform-unihiker)
- [Hermes Agent API documentation](https://hermes-agent.nousresearch.com/docs/user-guide/features/api-server/)


## v0.1.1 — Boot-loop diagnostic patch

This update is **not yet tested on a physical K10**. It retains the v0.1
features but adds boot checkpoints on USB serial before and after SD, ASR,
and Wi-Fi initialization, and avoids large temporary graphics buffers caused
by redundant `autoClean=true` calls. Neither the PC bridge nor the Hermes
configuration needs to change.

**Important:** Copy your previous `firmware/include/secrets.h` into this
new project's `firmware/include/` directory. It is intentionally excluded
from the zip. Do not copy your old `main.cpp` over the patched one.

1. Open this version's `firmware/` project in PlatformIO.
2. Plug the K10 **directly** into a reliable USB port. Open PlatformIO
   Serial Monitor at **115200 baud**. On Windows/VS Code you can use
   `pio device monitor -b 115200` in the PlatformIO terminal, or the
   PlatformIO Monitor action.
3. Press the board reset button and read the last `[ARLO]` line before
   it reboots. Preserve any `Guru Meditation Error`, `Backtrace:`,
   `Brownout detector`, `rst:...` or `abort()` lines.
4. Try one reboot **without the microSD card**. It should display
   `Insert FAT32 microSD` and remain running; audio can't work without SD.
5. If it still reboots, change one flag at a time in `platformio.ini`,
   then **rebuild and reflash**:
   - `-DARLO_ENABLE_SD=0`: skip SD access.
   - `-DARLO_ENABLE_ASR=0`: skip the speech recognition engine.
   - `-DARLO_ENABLE_WIFI=0`: skip Wi-Fi startup/retries.
   With a flag at 0, the screen says `Diagnostics build`. This is intentional.
6. Once isolated, restore all flags to `1`. Send the serial output and
   the disabled-flag result so the underlying issue can be fixed.

**Interpreting the logs:**

| Last checkpoint | Likely failing code |
| --- | --- |
| `04 AFTER first face` or `05 BEFORE SD.begin` | SD mount / TF card hardware |
| `06 SD.begin OK / BEFORE initSDFile` | K10 SDK's second SD mount or LVGL SD registration |
| `08 BEFORE asr.asrInit` | Wake-word model/recognizer initialization |
| `09 AFTER asr.asrInit` | Recognizer startup/model readiness |
| `11 BEFORE WiFi.mode` or `12 BEFORE WiFi.begin` | Wi-Fi initialization |
| `14 SETUP COMPLETED` | Failure occurs in `loop()`, not in boot setup |

DFRobot's official K10 troubleshooting notes that some TF cards crash when
combined with AI and that a damaged internal voice model may require
restoring device initialization settings. Don't restore settings until
serial diagnostics isolate ASR (restoring changes the device setup).
Sources: https://www.unihiker.com/wiki/K10/faq/
and https://github.com/DFRobot/framework-arduinounihiker/blob/master/libraries/unihiker_k10/src/unihiker_k10.cpp
