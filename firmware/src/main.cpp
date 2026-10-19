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
#include "face_animation.h"
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
constexpr uint32_t OUTLINE = 0x2A1723;
constexpr uint32_t FACE_AMBER = 0xF6A63A;
constexpr uint32_t FACE_GOLD = 0xFFD36B;
constexpr uint32_t FACE_ERROR = 0xE9783D;
constexpr uint32_t EYE_CREAM = 0xFFF1C7;
constexpr uint32_t MOUTH_WINE = 0x5B1E2A;
constexpr uint32_t WHITE = 0xFFF8E8;
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
arloface::FaceModel renderedModel;
bool renderedFace = false;
unsigned long lastWiFiAttempt = 0;
unsigned long lastRedraw = 0;
unsigned long lastBlinked = 0;
unsigned long wakeCooldownUntil = 0;

uint32_t faceFillColor(const String &state) {
    if (state == "error") return FACE_ERROR;
    if (state == "listening") return FACE_GOLD;
    return FACE_AMBER;
}

arloface::FaceState faceStateModel(const String &state) {
    if (state == "listening") return arloface::FaceState::Listening;
    if (state == "thinking") return arloface::FaceState::Thinking;
    if (state == "speaking") return arloface::FaceState::Speaking;
    if (state == "error") return arloface::FaceState::Error;
    return arloface::FaceState::Idle;
}

arloface::Gaze faceGaze(const String &state, uint8_t animationFrame) {
    if (state == "thinking") {
        switch (animationFrame % 4) {
            case 0: return arloface::Gaze::Up;
            case 1: return arloface::Gaze::Left;
            case 2: return arloface::Gaze::Right;
            default: return arloface::Gaze::Center;
        }
    }
    if (state == "error") return arloface::Gaze::Left;
    return arloface::Gaze::Center;
}

uint8_t thinkingMouthFrame(uint8_t animationFrame) {
    switch (animationFrame % 4) {
        case 0: return 5;  // slight curl
        case 1: return 6;  // lower-lip bite
        case 2: return 5;
        default: return 0;
    }
}

arloface::FaceModel makeFaceModel(const String &state, bool blink,
                                  uint8_t mouthFrame) {
    uint8_t animationFrame = state == "thinking"
        ? static_cast<uint8_t>((millis() / 650UL) % 4)
        : 0;
    arloface::FaceModel model;
    model.state = faceStateModel(state);
    model.gaze = faceGaze(state, animationFrame);
    model.blinkPhase = blink ? 1 : 0;
    model.mouthFrame = (state == "thinking" && mouthFrame == 0)
        ? thinkingMouthFrame(animationFrame)
        : mouthFrame;
    return model;
}

void clearFaceRegion(int x, int y, int width, int height, uint32_t fill) {
    k10.canvas->canvasRectangle(x, y, width, height, fill, fill, true);
}

void drawClockTicks() {
    const int starts[][2] = {
        {120, 58}, {151, 65}, {176, 87}, {195, 116},
        {201, 158}, {195, 200}, {176, 229}, {151, 251},
        {120, 258}, {89, 251}, {64, 229}, {45, 200},
        {39, 158}, {45, 116}, {64, 87}, {89, 65},
    };
    const int ends[][2] = {
        {120, 67}, {148, 73}, {170, 93}, {187, 119},
        {193, 158}, {187, 197}, {170, 223}, {148, 243},
        {120, 249}, {92, 243}, {70, 223}, {53, 197},
        {47, 158}, {53, 119}, {70, 93}, {92, 73},
    };
    for (uint8_t i = 0; i < 16; ++i) {
        k10.canvas->canvasLine(starts[i][0], starts[i][1],
                               ends[i][0], ends[i][1], OUTLINE);
    }
}

void drawStaticFace(const String &state) {
    uint32_t fill = faceFillColor(state);
    k10.canvas->canvasCircle(120, 158, 96, OUTLINE, OUTLINE, true);
    k10.canvas->canvasCircle(120, 158, 89, fill, fill, true);
    k10.canvas->canvasCircle(120, 158, 84, fill, fill, true);
    drawClockTicks();
    k10.canvas->canvasLine(42, 158, 49, 158, OUTLINE);
    k10.canvas->canvasLine(191, 158, 198, 158, OUTLINE);
    k10.canvas->canvasLine(61, 103, 66, 108, OUTLINE);
    k10.canvas->canvasLine(174, 208, 179, 213, OUTLINE);
}

void clearEyeRegion(uint32_t fill) {
    k10.canvas->canvasCircle(75, 130, 31, fill, fill, true);
    k10.canvas->canvasCircle(165, 130, 31, fill, fill, true);
}

void drawEyes(const String &state, arloface::Gaze gaze, uint8_t blinkPhase) {
    if (blinkPhase != 0) {
        k10.canvas->canvasLine(49, 130, 101, 130, OUTLINE);
        k10.canvas->canvasLine(139, 130, 191, 130, OUTLINE);
        return;
    }

    int lookX = 0;
    int lookY = 0;
    if (gaze == arloface::Gaze::Left) lookX = -7;
    if (gaze == arloface::Gaze::Right) lookX = 7;
    if (gaze == arloface::Gaze::Up) lookY = -5;
    int eyeRadius = state == "listening" ? 29 : 27;

    for (int x : {75, 165}) {
        k10.canvas->canvasCircle(x, 130, eyeRadius + 3, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(x, 130, eyeRadius, EYE_CREAM, EYE_CREAM, true);
        k10.canvas->canvasCircle(x + lookX, 130 + lookY, 10, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(x + lookX - 3, 126 + lookY, 3, WHITE, WHITE, true);
    }
    if (state == "thinking") {
        // Raised, slightly uneven brows read as concentration rather than anger.
        k10.canvas->canvasLine(49, 101, 94, 96, OUTLINE);
        k10.canvas->canvasLine(146, 96, 191, 101, OUTLINE);
    } else {
        k10.canvas->canvasLine(49, 101, 94, 97, OUTLINE);
        k10.canvas->canvasLine(146, 97, 191, 101, OUTLINE);
    }
}

void drawCaption(const String &caption) {
    clearFaceRegion(10, 262, 220, 32, INK);
    k10.canvas->canvasText(caption.substring(0, 23), 14, 275,
                          WHITE, k10.canvas->eCNAndENFont16, 210, false);
}

void drawMouth(const String &state, uint8_t mouthFrame) {
    (void)state;
    if (mouthFrame == 0) {
        k10.canvas->canvasLine(101, 207, 111, 211, OUTLINE);
        k10.canvas->canvasLine(111, 211, 120, 212, OUTLINE);
        k10.canvas->canvasLine(120, 212, 129, 211, OUTLINE);
        k10.canvas->canvasLine(129, 211, 139, 207, OUTLINE);
    } else if (mouthFrame == 1) {
        k10.canvas->canvasCircle(120, 207, 8, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(120, 207, 4, MOUTH_WINE, MOUTH_WINE, true);
    } else if (mouthFrame == 2) {
        k10.canvas->canvasCircle(120, 207, 15, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(120, 207, 10, MOUTH_WINE, MOUTH_WINE, true);
        k10.canvas->canvasLine(112, 214, 128, 214, FACE_GOLD);
    } else if (mouthFrame == 3) {
        k10.canvas->canvasRectangle(99, 195, 42, 25, OUTLINE, OUTLINE, true);
        k10.canvas->canvasRectangle(105, 201, 30, 13, MOUTH_WINE, MOUTH_WINE, true);
        k10.canvas->canvasLine(108, 215, 132, 215, FACE_GOLD);
    } else if (mouthFrame == 4) {
        k10.canvas->canvasCircle(120, 207, 12, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(120, 207, 7, MOUTH_WINE, MOUTH_WINE, true);
    } else if (mouthFrame == 5) {
        // A small asymmetric curl for a concentrated, thinking expression.
        k10.canvas->canvasLine(103, 210, 112, 207, OUTLINE);
        k10.canvas->canvasLine(112, 207, 121, 211, OUTLINE);
        k10.canvas->canvasLine(121, 211, 137, 205, OUTLINE);
    } else if (mouthFrame == 6) {
        // The gold lower lip crossing the dark mouth suggests a gentle bite.
        k10.canvas->canvasCircle(120, 208, 10, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(120, 211, 6, MOUTH_WINE, MOUTH_WINE, true);
        k10.canvas->canvasLine(109, 203, 131, 203, FACE_GOLD);
    } else {
        k10.canvas->canvasCircle(120, 207, 12, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(120, 207, 7, MOUTH_WINE, MOUTH_WINE, true);
    }
}

void renderFullFace(const String &state, const String &caption,
                    const arloface::FaceModel &model) {
    uint32_t fill = faceFillColor(state);
    k10.canvas->canvasClear();
    drawStaticFace(state);
    drawEyes(state, model.gaze, model.blinkPhase);
    drawMouth(state, model.mouthFrame);
    k10.canvas->canvasText("ARLO MINI", 57, 29, fill,
                          k10.canvas->eCNAndENFont16, 100, false);
    drawCaption(caption);
    renderedState = state;
    renderedCaption = caption;
    renderedModel = model;
    renderedFace = true;
}

void face(const String &state, const String &caption, bool blink = false,
          uint8_t mouthFrame = 0) {
    faceState = state;
    statusText = caption;
    if (!displayReady || !k10.canvas) return;

    arloface::FaceModel next = makeFaceModel(state, blink, mouthFrame);
    bool captionChanged = !renderedFace || renderedCaption != caption;
    arloface::FaceDirtyRegion dirty = arloface::diff(
        renderedModel, next, renderedFace, captionChanged);
    if (dirty.full) {
        renderFullFace(state, caption, next);
    } else {
        uint32_t fill = faceFillColor(state);
        if (dirty.eyes) {
            clearEyeRegion(fill);
            drawEyes(state, next.gaze, next.blinkPhase);
        }
        if (dirty.mouth) {
            clearFaceRegion(94, 188, 52, 43, fill);
            drawMouth(state, next.mouthFrame);
        }
        if (dirty.caption) {
            drawCaption(caption);
        }
        renderedCaption = caption;
        renderedModel = next;
    }
    if (!dirty.any()) return;
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
                face("thinking", state + "...", false, 0);
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

    face("speaking", "ARLO speaking", false, 2);
    music.playTFCardAudio("S:/arlo-reply.wav");

    // Follow the SDK playback task, but also enforce the WAV duration. The SDK
    // can report idle early, and starting recording immediately would make the
    // next listening turn compete with the tail of the current response.
    unsigned long began = millis();
    unsigned long nextMouthToggle = began + 260;
    bool started = false;
    uint8_t mouthFrame = 2;
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
            mouthFrame = static_cast<uint8_t>((mouthFrame + 1) % 5);
            face("speaking", "ARLO speaking", false, mouthFrame);
            nextMouthToggle += 260;
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
