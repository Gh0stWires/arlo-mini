// ARLO Mini v0.1 — hands-free K10 companion.
// Wake with built-in EN_MODE phrase "Jarvis" / "Hi Telly" (not yet "Hey ARLO").
// Requires FAT32 microSD and bridge server running on local Wi-Fi.

#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include "unihiker_k10.h"
#include "asr.h"
#include "secrets.h"

#ifndef WIFI_FORCE_BSSID
#define WIFI_FORCE_BSSID 0
#endif
#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 0
#endif

// Diagnose a boot loop without editing application logic.
// Disable one subsystem at a time via firmware/platformio.ini.
#ifndef ARLO_ENABLE_SD
#define ARLO_ENABLE_SD 1
#endif
#ifndef ARLO_ENABLE_ASR
#define ARLO_ENABLE_ASR 1
#endif
#ifndef ARLO_ENABLE_WIFI
#define ARLO_ENABLE_WIFI 1
#endif
#ifndef ARLO_HOLD_SCREEN
#define ARLO_HOLD_SCREEN 0
#endif

void checkpoint(const char *step) {
    Serial.printf("[ARLO][%lu ms] %s | heap=%u psram=%u\n", millis(), step,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    Serial.flush();
    delay(90);  // Let the USB serial adapter transmit before a possible reset.
}


UNIHIKER_K10 k10;
Music music;
ASR asr;

constexpr uint32_t INK = 0x07111B;
constexpr uint32_t CYAN = 0x37E6E9;
constexpr uint32_t DIM = 0x155866;
constexpr uint32_t WHITE = 0xDCFDFD;
constexpr uint32_t AMBER = 0xFFB94D;
constexpr uint8_t RECORD_SECONDS = 5;
constexpr int MAX_FOLLOWUPS = 2;
constexpr size_t MIN_WAV_BYTES = 1000;
constexpr unsigned long JOB_TIMEOUT_MS = 250000;
constexpr unsigned long RECONNECT_EVERY_MS = 8000;
const char *INPUT_WAV = "/request.wav";
const char *OUTPUT_WAV = "/arlo-reply.wav";

String statusText = "STARTING";
String faceState = "idle";
bool sdReady = false;
bool voiceReady = false;
bool displayReady = false;
bool wasAwake = false;
String renderedState;
String renderedCaption;
bool renderedBlink = false;
bool renderedMouthOpen = false;
bool renderedFace = false;
unsigned long lastWiFiAttempt = 0;
unsigned long lastRedraw = 0;
unsigned long lastBlinked = 0;
unsigned long wakeCooldownUntil = 0;

uint32_t faceColor(const String &state) {
    return state == "error" ? AMBER : CYAN;
}

void clearFaceRegion(int x, int y, int width, int height) {
    k10.canvas->canvasRectangle(x, y, width, height, INK, INK, true);
}

void drawEyes(const String &state, bool blink) {
    uint32_t color = faceColor(state);
    if (blink) {
        k10.canvas->canvasRectangle(35, 124, 70, 5, color, color, true);
        k10.canvas->canvasRectangle(135, 124, 70, 5, color, color, true);
        return;
    }
    int radius = state == "listening" ? 30 : 27;
    int look = state == "thinking" ? 9 : 0;
    k10.canvas->canvasCircle(75 + look, 124, radius, color, color, true);
    k10.canvas->canvasCircle(165 + look, 124, radius, color, color, true);
    k10.canvas->canvasCircle(79 + look, 120, 8, WHITE, WHITE, true);
    k10.canvas->canvasCircle(169 + look, 120, 8, WHITE, WHITE, true);
}

void drawMouth(const String &state, bool mouthOpen) {
    uint32_t color = faceColor(state);
    if (mouthOpen) {
        k10.canvas->canvasCircle(120, 213, 15, color, color, true);
    } else {
        k10.canvas->canvasLine(103, 207, 137, 207, DIM);
    }
}

void renderFullFace(const String &state, const String &caption,
                    bool blink, bool mouthOpen) {
    uint32_t color = faceColor(state);
    k10.canvas->canvasClear();
    drawEyes(state, blink);
    drawMouth(state, mouthOpen);
    k10.canvas->canvasText("ARLO MINI", 57, 29, color,
                          k10.canvas->eCNAndENFont16, 100, false);
    k10.canvas->canvasText(caption.substring(0, 23), 14, 275,
                          WHITE, k10.canvas->eCNAndENFont16, 210, false);
    renderedState = state;
    renderedCaption = caption;
    renderedBlink = blink;
    renderedMouthOpen = mouthOpen;
    renderedFace = true;
}

void face(const String &state, const String &caption, bool blink = false, bool mouthOpen = false) {
    faceState = state;
    statusText = caption;
    if (!displayReady || !k10.canvas) return;

    bool baseChanged = !renderedFace || renderedState != state || renderedCaption != caption;
    if (baseChanged) {
        renderFullFace(state, caption, blink, mouthOpen);
    } else {
        if (renderedBlink != blink) {
            clearFaceRegion(30, 82, 180, 86);
            drawEyes(state, blink);
            renderedBlink = blink;
        }
        if (renderedMouthOpen != mouthOpen) {
            clearFaceRegion(95, 190, 50, 50);
            drawMouth(state, mouthOpen);
            renderedMouthOpen = mouthOpen;
        }
    }
    digital_write(eLCD_BLK, 1);
    k10.canvas->updateCanvas();
    lastRedraw = millis();
}

void initDisplay() {
    k10.initScreen(2);
    digital_write(eLCD_BLK, 1);
    k10.creatCanvas();
    k10.setScreenBackground(INK);
    displayReady = true;
}

void connectWifi() {
#if ARLO_ENABLE_WIFI
    if (WiFi.status() == WL_CONNECTED) return;
    if (millis() - lastWiFiAttempt < RECONNECT_EVERY_MS && lastWiFiAttempt != 0) return;
    lastWiFiAttempt = millis();
    face("thinking", "Connecting Wi-Fi...");
    WiFi.mode(WIFI_STA);
#if WIFI_FORCE_BSSID
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL, WIFI_BSSID, true);
    Serial.printf("WiFi connect requested on channel %d / pinned BSSID\n", WIFI_CHANNEL);
#else
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("WiFi connect requested");
#endif
#endif
}

void setCommonHeaders(HTTPClient &http) {
    http.addHeader("X-Device-Key", DEVICE_KEY);
}

// POST WAV by streaming from microSD, without allocating the whole audio in RAM.
String createJob() {
    for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
        File audio = SD.open(INPUT_WAV, FILE_READ);
        if (!audio || audio.size() < MIN_WAV_BYTES) {
            Serial.println("Audio file missing or too small");
            if (audio) audio.close();
            return "";
        }
        HTTPClient http;
        http.begin(String(BRIDGE_BASE_URL) + "/turn");
        http.setReuse(false);
        http.useHTTP10(true);
        http.setConnectTimeout(10000);
        http.setTimeout(30000);
        setCommonHeaders(http);
        http.addHeader("Content-Type", "audio/wav");
        int status = http.sendRequest("POST", &audio, audio.size());
        audio.close();
        String payload = status == 202 ? http.getString() : "";
        Serial.printf("POST /turn attempt=%u -> %d %s\n",
                      attempt, status, payload.c_str());
        http.end();
        if (status == 202) {
            DynamicJsonDocument doc(1024);
            if (!deserializeJson(doc, payload)) return doc["id"].as<String>();
            return "";
        }
        delay(300 * attempt);
    }
    Serial.println("POST /turn failed after retries");
    return "";
}

// Polling lets us animate while Hermes/Whisper/TTS are running on the PC.
String pollJob(const String &id) {
    unsigned long start = millis();
    unsigned long frames = 0;
    while (millis() - start < JOB_TIMEOUT_MS) {
        if (WiFi.status() != WL_CONNECTED) return "error";
        HTTPClient http;
        http.begin(String(BRIDGE_BASE_URL) + "/jobs/" + id);
        http.setTimeout(5000);
        setCommonHeaders(http);
        int code = http.GET();
        if (code == 200) {
            DynamicJsonDocument doc(1536);
            String response = http.getString();
            if (deserializeJson(doc, response) == DeserializationError::Ok) {
                String state = doc["state"].as<String>();
                if (state == "ready" || state == "no_speech" || state == "error") {
                    if (state == "error") Serial.println(doc["error"].as<String>());
                    http.end();
                    return state;
                }
                face("thinking", state + "...", false, (frames++ % 2) == 0);
            }
        } else {
            Serial.printf("GET status -> %d\n", code);
        }
        http.end();
        delay(700);
    }
    return "error";
}

bool fetchAudio(const String &id) {
    const uint8_t maxAttempts = 3;
    for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        bool removed = SD.remove(OUTPUT_WAV);
        if (attempt == 1) {
            Serial.printf("SD before audio total=%llu used=%llu free=%llu removed=%d\n",
                          (unsigned long long)SD.totalBytes(),
                          (unsigned long long)SD.usedBytes(),
                          (unsigned long long)(SD.totalBytes() - SD.usedBytes()),
                          removed ? 1 : 0);
        }

        File out = SD.open(OUTPUT_WAV, FILE_WRITE);
        if (!out) {
            Serial.printf("Audio open failed attempt=%u\n", attempt);
            delay(300 * attempt);
            continue;
        }

        HTTPClient http;
        http.begin(String(BRIDGE_BASE_URL) + "/jobs/" + id + "/audio");
        http.setReuse(false);
        http.useHTTP10(true);
        http.setConnectTimeout(10000);
        http.setTimeout(60000);
        setCommonHeaders(http);
        int status = http.GET();
        bool success = false;
        int expected = -1;
        int downloaded = 0;
        size_t stored = 0;

        if (status == 200) {
            WiFiClient *stream = http.getStreamPtr();
            expected = http.getSize();
            uint8_t buffer[4096];
            unsigned long lastData = millis();
            while (stream && (expected < 0 || downloaded < expected) &&
                   millis() - lastData < 60000UL) {
                int available = stream->available();
                if (available <= 0) {
                    delay(5);
                    continue;
                }
                size_t wanted = min((size_t)available, sizeof(buffer));
                size_t received = stream->readBytes(buffer, wanted);
                if (received == 0) continue;
                size_t written = out.write(buffer, received);
                if (written != received) {
                    Serial.printf("Audio SD short write %u/%u attempt=%u\n",
                                  (unsigned)written, (unsigned)received, attempt);
                    break;
                }
                downloaded += (int)written;
                lastData = millis();
                if ((downloaded % 65536) < (int)written) out.flush();
            }
            out.flush();
            out.close();
            File check = SD.open(OUTPUT_WAV, FILE_READ);
            stored = check ? check.size() : 0;
            if (check) check.close();
            success = stored > MIN_WAV_BYTES &&
                      (expected < 0 || stored == (size_t)expected);
            Serial.printf("Audio download attempt=%u network=%d expected=%d SD=%u success=%d\n",
                          attempt, downloaded, expected, (unsigned)stored,
                          success ? 1 : 0);
        } else {
            Serial.printf("Audio download attempt=%u status=%d\n", attempt, status);
            out.close();
        }
        http.end();
        if (success) return true;
        SD.remove(OUTPUT_WAV);
        delay(300 * attempt);
    }
    Serial.println("Audio download failed after retries");
    return false;
}

unsigned long outputWavDurationMs() {
    File f = SD.open(OUTPUT_WAV, FILE_READ);
    if (!f) return 0;
    size_t bytes = f.size();
    uint32_t byteRate = 64000;
    if (bytes >= 32) {
        f.seek(28);
        uint8_t rate[4] = {0, 0, 0, 0};
        if (f.read(rate, sizeof(rate)) == sizeof(rate)) {
            byteRate = (uint32_t)rate[0] |
                       ((uint32_t)rate[1] << 8) |
                       ((uint32_t)rate[2] << 16) |
                       ((uint32_t)rate[3] << 24);
        }
    }
    f.close();
    if (bytes <= 44 || byteRate == 0) return 0;
    return (unsigned long)(((bytes - 44ULL) * 1000ULL) / byteRate);
}

void playback() {
    if (!SD.exists(OUTPUT_WAV)) return;
    unsigned long expectedMs = outputWavDurationMs();
    unsigned long minimumPlaybackMs = expectedMs + 750UL;
    unsigned long timeoutMs = max(minimumPlaybackMs + 15000UL, 30000UL);
    Serial.printf("Playback WAV duration=%lu ms minimum=%lu ms\n",
                  expectedMs, minimumPlaybackMs);

    face("speaking", "ARLO speaking", false, true);
    music.playTFCardAudio("S:/arlo-reply.wav");

    // Follow the SDK playback task, but also enforce the WAV duration. The SDK
    // can report idle early, and starting recording immediately would make the
    // next listening turn compete with the tail of the current response.
    unsigned long began = millis();
    unsigned long nextMouthToggle = began + 300;
    bool started = false;
    bool open = true;
    while (millis() - began < timeoutMs) {
        bool playing = k10.getPlayMusicState() != 0;
        if (playing) {
            started = true;
        }

        unsigned long elapsed = millis() - began;
        bool sdkFinished = started && !playing;
        bool durationFinished = elapsed >= minimumPlaybackMs;
        if ((sdkFinished && durationFinished) ||
            (!started && durationFinished && expectedMs == 0)) {
            break;
        }
        if (!started && elapsed > 3000 && expectedMs == 0) {
            Serial.println("Playback task did not start");
            break;
        }

        unsigned long now = millis();
        if (now >= nextMouthToggle) {
            open = !open;
            face("speaking", "ARLO speaking", false, open);
            nextMouthToggle += 300;
        }
        delay(35);
    }
    if (millis() - began >= timeoutMs && k10.getPlayMusicState() != 0) {
        Serial.println("Playback timeout; stopping stuck audio task");
        music.stopPlayAudio();
    }
}

void voiceTurn(bool followup) {
    face("listening", followup ? "Anything else?" : "Listening now");
    // SDK recording is fixed-duration. VAD and interruption are future milestones.
    SD.remove(INPUT_WAV);
    music.recordSaveToTFCard("S:/request.wav", RECORD_SECONDS);
    face("thinking", "Sending audio...");
    String job = createJob();
    if (job.isEmpty()) {
        face("error", "Upload failed");
        delay(2000);
        return;
    }
    String outcome = pollJob(job);
    if (outcome == "no_speech") {
        face("idle", "Say Jarvis to talk");
        return;
    }
    if (outcome != "ready") {
        face("error", "Bridge error - see USB");
        delay(2000);
        return;
    }
    face("thinking", "Getting voice...");
    if (!fetchAudio(job)) {
        face("error", "Download failed");
        delay(2000);
        return;
    }
    playback();
}

void conversation() {
    if (!sdReady || WiFi.status() != WL_CONNECTED) {
        face("error", "Check SD / Wi-Fi");
        return;
    }
    wakeCooldownUntil = millis() + 6000;
    // First turn plus two automatic follow-up recording windows. No buttons.
    // A silence turn exits. Wake detector is NOT consulted during conversation.
    for (int i = 0; i <= MAX_FOLLOWUPS; ++i) {
        bool followup = i > 0;
        voiceTurn(followup);
        // After each turn, continue only when turn succeeded. Our prototype
        // checks the state label: on silence/error it becomes idle/error.
        if (faceState == "idle" || faceState == "error") break;
        if (i < MAX_FOLLOWUPS) delay(450);
    }
    face("idle", "Say Jarvis to talk");
    wakeCooldownUntil = millis() + 2000;
}

void setup() {
    Serial.begin(115200);
    delay(600);
    Serial.printf("\n[ARLO] firmware v0.1.1 diagnostic; reset reason=%d\n",
                  (int)esp_reset_reason());
    checkpoint("01 BEFORE k10.begin");
    k10.begin();
    checkpoint("02 AFTER k10.begin / BEFORE subsystems");
    initDisplay();
    face("thinking", "Booting ARLO...");
#if ARLO_SCREEN_SMOKE
    // Minimal display-only probe. It intentionally stops before SD/ASR/Wi-Fi.
    initDisplay();
    face("idle", "SCREEN TEST 2");
    Serial.println("[ARLO] SCREEN SMOKE ACTIVE");
    while (true) delay(1000);
#endif

#if ARLO_ENABLE_SD
    face("thinking", "Checking microSD...");
    checkpoint("05 BEFORE SD.begin");
    if (SD.begin()) {
        checkpoint("06 SD.begin OK / BEFORE initSDFile");
        // K10 SDK registers its 'S:/' filesystem in initSDFile().
        // WARNING: internally initSDFile() mounts SD again and retries forever
        // on failure. Log both stages separately so a stuck mount is visible.
        k10.initSDFile();
        sdReady = true;
        checkpoint("07 AFTER initSDFile");
    } else {
        sdReady = false;
        checkpoint("06 SD.begin FAILED (card absent, incompatible or filesystem)");
        face("error", "Insert FAT32 microSD");
    }
#else
    checkpoint("05 SD DISABLED (diagnostic build)");
#endif

#if ARLO_ENABLE_ASR
    face("thinking", "Initializing ASR...");
    checkpoint("08 BEFORE asr.asrInit");
    asr.asrInit(CONTINUOUS, EN_MODE, 6000);
    checkpoint("09 AFTER asr.asrInit / WAITING for model");
    unsigned long started = millis();
    while (asr._asrState == 0 && millis() - started < 12000) delay(100);
    voiceReady = asr._asrState != 0;
    checkpoint(voiceReady ? "10 ASR READY" : "10 ASR DID NOT BECOME READY");
#else
    checkpoint("08 ASR DISABLED (diagnostic build)");
#endif

    // Allocate the LVGL canvas only after ASR has finished using PSRAM.
    checkpoint("10.5 BEFORE display init");
    initDisplay();
    face("thinking", "Starting ARLO...");

#if ARLO_ENABLE_WIFI
    checkpoint("11 BEFORE WiFi.mode");
    WiFi.mode(WIFI_STA);
    checkpoint("12 BEFORE WiFi.begin");
#if WIFI_FORCE_BSSID
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL, WIFI_BSSID, true);
    Serial.printf("WiFi connect requested on channel %d / pinned BSSID\n", WIFI_CHANNEL);
#else
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("WiFi connect requested");
#endif
    lastWiFiAttempt = millis();
    checkpoint("13 AFTER WiFi.begin");
#else
    checkpoint("11 WIFI DISABLED (diagnostic build)");
#endif

#if !ARLO_ENABLE_SD || !ARLO_ENABLE_ASR || !ARLO_ENABLE_WIFI
    String diagnostic = "SD ";
    diagnostic += sdReady ? "OK" : "OFF";
    diagnostic += " ASR ";
    diagnostic += voiceReady ? "OK" : "OFF";
    diagnostic += " WIFI ";
    diagnostic += ARLO_ENABLE_WIFI ? "ON" : "OFF";
    face("idle", diagnostic);
#else
    face(sdReady && voiceReady ? "idle" : "error",
         !sdReady ? "Insert microSD" : (!voiceReady ? "ASR init failed" : "Say Jarvis to talk"));
#endif
#if ARLO_HOLD_SCREEN
    face("idle", "ASR SCREEN HOLD");
    Serial.println("[ARLO] SCREEN HOLD ACTIVE");
    while (true) delay(1000);
#endif
    checkpoint("14 SETUP COMPLETED");
}

void loop() {
    connectWifi();
    if (!sdReady || !voiceReady) {
        delay(1000);
        return;
    }
    bool awake = asr.isWakeUp();
    if (!awake) wasAwake = false;
    if (awake && !wasAwake && millis() > wakeCooldownUntil) {
        wasAwake = true;
        conversation();
    }
    // Idle blinking. Face is not redrawn while network/audio is running.
    if (millis() - lastRedraw > 3500) {
        face("idle", WiFi.status() == WL_CONNECTED ? "Say Jarvis to talk" : "Wi-Fi disconnected", true);
        lastBlinked = millis();
    } else if (lastBlinked > 0 && millis() - lastBlinked > 140 && millis() - lastBlinked < 800) {
        face("idle", WiFi.status() == WL_CONNECTED ? "Say Jarvis to talk" : "Wi-Fi disconnected");
        lastBlinked = 0;
    }
    delay(55);
}
