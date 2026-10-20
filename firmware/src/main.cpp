// ARLO Mini v0.1 — hands-free K10 companion.
// Wake with built-in EN_MODE phrase "Jarvis" / "Hi Telly" (not yet "Hey ARLO").
// Requires FAT32 microSD and bridge server running on local Wi-Fi.

#include <Arduino.h>
#include <math.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include "unihiker_k10.h"
#include "asr.h"
#include "face_animation.h"
#include "voice_activity.h"
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
extern SemaphoreHandle_t xSPIlMutex;

constexpr uint32_t INK = 0x07111B;
constexpr uint32_t OUTLINE = 0x2A1723;
constexpr uint32_t FACE_AMBER = 0xF6A63A;
constexpr uint32_t FACE_GOLD = 0xFFD36B;
constexpr uint32_t FACE_ERROR = 0xE9783D;
constexpr uint32_t EYE_CREAM = 0xFFF1C7;
constexpr uint32_t MOUTH_WINE = 0x5B1E2A;
constexpr uint32_t WHITE = 0xFFF8E8;
constexpr uint16_t VAD_FRAME_MS = 20;
constexpr uint16_t VAD_FRAME_BYTES = 1280;
constexpr uint16_t VAD_PREROLL_FRAMES = 12;
constexpr uint32_t VAD_MAX_UTTERANCE_MS = 30000;
constexpr unsigned long CONVERSATION_SESSION_MS = 300000UL;
constexpr size_t MIN_WAV_BYTES = 1000;
constexpr unsigned long JOB_TIMEOUT_MS = 250000;
constexpr unsigned long RECONNECT_EVERY_MS = 8000;
const char *INPUT_WAV = "/request.wav";
const char *OUTPUT_WAV = "/arlo-reply.wav";
constexpr uint16_t AUDIO_ENVELOPE_BINS = 1024;
uint8_t audioEnvelope[AUDIO_ENVELOPE_BINS];
uint32_t audioEnvelopeSum[AUDIO_ENVELOPE_BINS];
uint16_t audioEnvelopeCount = 0;

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
bool previousInterruptionWake = false;
unsigned long interruptionIgnoreUntil = 0;

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
    if (state == "speaking") {
        switch (animationFrame % 6) {
            case 1: return arloface::Gaze::Right;
            case 3: return arloface::Gaze::Left;
            case 4: return arloface::Gaze::Up;
            default: return arloface::Gaze::Center;
        }
    }
    if (state == "error") return arloface::Gaze::Left;
    return arloface::Gaze::Center;
}

uint8_t thinkingMouthFrame(uint8_t animationFrame) {
    switch (animationFrame % 4) {
        case 0: return 5;  // slight curl
        case 1: return 0;  // relax
        case 2: return 5;
        default: return 0;
    }
}

arloface::FaceModel makeFaceModel(const String &state, bool blink,
                                  uint8_t mouthFrame) {
    uint8_t animationFrame = 0;
    if (state == "thinking") {
        animationFrame = static_cast<uint8_t>((millis() / 650UL) % 4);
    } else if (state == "speaking") {
        animationFrame = static_cast<uint8_t>((millis() / 900UL) % 6);
    }
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
    constexpr float DEGREES_TO_RADIANS = 0.01745329252f;
    constexpr int CENTER_X = 120;
    constexpr int CENTER_Y = 158;
    constexpr int MINUTE_OUTER_RADIUS = 85;
    constexpr int MINUTE_INNER_RADIUS = 81;
    constexpr int HOUR_OUTER_RADIUS = 87;
    constexpr int HOUR_INNER_RADIUS = 75;

    for (uint8_t minute = 0; minute < 60; ++minute) {
        bool hourMarker = minute % 5 == 0;
        float angle = (static_cast<float>(minute) * 6.0f - 90.0f) *
                      DEGREES_TO_RADIANS;
        int outerRadius = hourMarker ? HOUR_OUTER_RADIUS
                                     : MINUTE_OUTER_RADIUS;
        int innerRadius = hourMarker ? HOUR_INNER_RADIUS
                                     : MINUTE_INNER_RADIUS;
        int outerX = static_cast<int>(lroundf(
            CENTER_X + cosf(angle) * outerRadius));
        int outerY = static_cast<int>(lroundf(
            CENTER_Y + sinf(angle) * outerRadius));
        int innerX = static_cast<int>(lroundf(
            CENTER_X + cosf(angle) * innerRadius));
        int innerY = static_cast<int>(lroundf(
            CENTER_Y + sinf(angle) * innerRadius));
        k10.canvas->canvasLine(outerX, outerY, innerX, innerY, OUTLINE);
    }
}

void drawStaticFace(const String &state) {
    uint32_t fill = faceFillColor(state);
    k10.canvas->canvasCircle(120, 158, 96, OUTLINE, OUTLINE, true);
    k10.canvas->canvasCircle(120, 158, 89, fill, fill, true);
    k10.canvas->canvasCircle(120, 158, 84, fill, fill, true);
    drawClockTicks();
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
        // A shallow open smile bridges naturally from the resting smile.
        k10.canvas->canvasLine(104, 207, 112, 210, OUTLINE);
        k10.canvas->canvasLine(112, 210, 120, 211, OUTLINE);
        k10.canvas->canvasLine(120, 211, 128, 210, OUTLINE);
        k10.canvas->canvasLine(128, 210, 136, 207, OUTLINE);
        k10.canvas->canvasLine(111, 209, 129, 209, MOUTH_WINE);
    } else if (mouthFrame == 2) {
        // A small open jaw gives the audio motion a second, distinct step.
        k10.canvas->canvasCircle(120, 208, 9, OUTLINE, OUTLINE, true);
        k10.canvas->canvasCircle(120, 208, 5, MOUTH_WINE, MOUTH_WINE, true);
        k10.canvas->canvasLine(113, 214, 127, 214, FACE_GOLD);
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

void armWakeInterruption() {
    previousInterruptionWake = asr.isWakeUp();
    interruptionIgnoreUntil = millis() + 1500UL;
}

bool checkWakeInterruption() {
    bool current = asr.isWakeUp();
    bool requested = false;
    if (millis() >= interruptionIgnoreUntil) {
        requested = current && !previousInterruptionWake;
    }
    previousInterruptionWake = current;
    return requested;
}

bool cancelBridgeJob(const String &id) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.begin(String(BRIDGE_BASE_URL) + "/jobs/" + id + "/cancel");
    http.setTimeout(5000);
    setCommonHeaders(http);
    int code = http.POST("");
    Serial.printf("POST /jobs/%s/cancel -> %d\n", id.c_str(), code);
    http.end();
    return code == 200;
}

void fillWavHeader(uint8_t *header, uint32_t dataBytes) {
    memset(header, 0, 44);
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    uint32_t fileBytes = dataBytes + 36;
    header[4] = fileBytes & 0xFF; header[5] = (fileBytes >> 8) & 0xFF;
    header[6] = (fileBytes >> 16) & 0xFF; header[7] = (fileBytes >> 24) & 0xFF;
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[20] = 1;
    header[22] = 2;
    uint32_t sampleRate = 16000;
    uint32_t byteRate = 64000;
    header[24] = sampleRate & 0xFF; header[25] = (sampleRate >> 8) & 0xFF;
    header[26] = (sampleRate >> 16) & 0xFF; header[27] = (sampleRate >> 24) & 0xFF;
    header[28] = byteRate & 0xFF; header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF; header[31] = (byteRate >> 24) & 0xFF;
    header[32] = 4; header[34] = 16;
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = dataBytes & 0xFF; header[41] = (dataBytes >> 8) & 0xFF;
    header[42] = (dataBytes >> 16) & 0xFF; header[43] = (dataBytes >> 24) & 0xFF;
}

uint16_t audioFrameRms(const uint8_t *buffer, size_t bytes) {
    uint64_t sumSquares = 0;
    uint32_t samples = 0;
    for (size_t offset = 0; offset + 3 < bytes; offset += 4) {
        int32_t left = static_cast<int16_t>(
            static_cast<uint16_t>(buffer[offset]) |
            (static_cast<uint16_t>(buffer[offset + 1]) << 8));
        int32_t right = static_cast<int16_t>(
            static_cast<uint16_t>(buffer[offset + 2]) |
            (static_cast<uint16_t>(buffer[offset + 3]) << 8));
        int32_t sample = (left + right) / 2;
        sumSquares += static_cast<uint64_t>(sample * sample);
        ++samples;
    }
    if (samples == 0) return 0;
    uint32_t rms = static_cast<uint32_t>(sqrtf(
        static_cast<float>(sumSquares) / samples));
    return static_cast<uint16_t>(min<uint32_t>(rms, 65535));
}

bool writeRecordingBytes(lv_fs_file_t *file, const uint8_t *data, size_t bytes) {
    xSemaphoreTake(xSPIlMutex, portMAX_DELAY);
    lv_fs_res_t result = lv_fs_write(file, data, bytes, nullptr);
    xSemaphoreGive(xSPIlMutex);
    return result == LV_FS_RES_OK;
}

bool recordUntilSpeechEnds() {
    constexpr size_t prerollBytes =
        static_cast<size_t>(VAD_PREROLL_FRAMES) * VAD_FRAME_BYTES;
    uint8_t *frame = static_cast<uint8_t *>(
        heap_caps_malloc(VAD_FRAME_BYTES, MALLOC_CAP_SPIRAM));
    uint8_t *preroll = static_cast<uint8_t *>(
        heap_caps_malloc(prerollBytes, MALLOC_CAP_SPIRAM));
    if (!frame || !preroll) {
        if (frame) heap_caps_free(frame);
        if (preroll) heap_caps_free(preroll);
        Serial.println("VAD capture buffer allocation failed");
        return false;
    }

    lv_fs_file_t file;
    xSemaphoreTake(xSPIlMutex, portMAX_DELAY);
    lv_fs_res_t openResult = lv_fs_open(&file, "S:/request.wav", LV_FS_MODE_WR);
    xSemaphoreGive(xSPIlMutex);
    if (openResult != LV_FS_RES_OK) {
        heap_caps_free(frame);
        heap_caps_free(preroll);
        Serial.println("VAD request WAV open failed");
        return false;
    }

    uint8_t header[44];
    fillWavHeader(header, 0);
    if (!writeRecordingBytes(&file, header, sizeof(header))) {
        xSemaphoreTake(xSPIlMutex, portMAX_DELAY);
        lv_fs_close(&file);
        xSemaphoreGive(xSPIlMutex);
        heap_caps_free(frame);
        heap_caps_free(preroll);
        Serial.println("VAD request WAV header write failed");
        return false;
    }

    arlo_voice::VoiceActivityConfig config;
    config.frameMs = VAD_FRAME_MS;
    config.maximumUtteranceMs = VAD_MAX_UTTERANCE_MS;
    arlo_voice::VoiceActivity activity(config);
    digital_write(eAmp_Gain, 1);
    uint32_t dataBytes = 0;
    uint32_t frameIndex = 0;
    uint16_t ringCount = 0;
    uint16_t ringWrite = 0;
    bool writing = false;
    bool failed = false;

    xSemaphoreTake(xI2SMutex, portMAX_DELAY);
    while (!failed) {
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_0, frame, VAD_FRAME_BYTES, &bytesRead, portMAX_DELAY);
        if (bytesRead != VAD_FRAME_BYTES) {
            Serial.printf("VAD short I2S frame %u/%u\n",
                          (unsigned)bytesRead, (unsigned)VAD_FRAME_BYTES);
            failed = true;
            break;
        }

        const uint32_t elapsedMs = frameIndex * VAD_FRAME_MS;
        const uint16_t rms = audioFrameRms(frame, bytesRead);
        arlo_voice::CaptureDecision decision = activity.push({elapsedMs, rms});
        memcpy(preroll + static_cast<size_t>(ringWrite) * VAD_FRAME_BYTES,
               frame, VAD_FRAME_BYTES);
        ringWrite = (ringWrite + 1) % VAD_PREROLL_FRAMES;
        if (ringCount < VAD_PREROLL_FRAMES) ++ringCount;

        if (!writing && activity.speechStarted()) {
            uint16_t oldest = (ringWrite + VAD_PREROLL_FRAMES - ringCount) %
                              VAD_PREROLL_FRAMES;
            for (uint16_t i = 0; i < ringCount; ++i) {
                uint16_t index = (oldest + i) % VAD_PREROLL_FRAMES;
                if (!writeRecordingBytes(&file,
                        preroll + static_cast<size_t>(index) * VAD_FRAME_BYTES,
                        VAD_FRAME_BYTES)) {
                    failed = true;
                    break;
                }
                dataBytes += VAD_FRAME_BYTES;
            }
            writing = !failed;
            if (writing) Serial.printf("VAD speech start rms=%u ms=%lu\n",
                                       rms, (unsigned long)activity.speechStartMs());
        } else if (writing) {
            if (!writeRecordingBytes(&file, frame, VAD_FRAME_BYTES)) {
                failed = true;
            } else {
                dataBytes += VAD_FRAME_BYTES;
            }
        }

        if ((frameIndex % 25) == 0) {
            Serial.printf("VAD frame ms=%lu rms=%u state=%u silence=%lu\n",
                          (unsigned long)elapsedMs, rms,
                          static_cast<unsigned>(decision),
                          (unsigned long)activity.trailingSilenceMs());
        }
        ++frameIndex;
        if (decision == arlo_voice::CaptureDecision::Finished ||
            decision == arlo_voice::CaptureDecision::InitialSilenceTimeout ||
            decision == arlo_voice::CaptureDecision::MaximumDuration) {
            break;
        }
    }
    xSemaphoreGive(xI2SMutex);

    bool success = false;
    if (!failed && writing && dataBytes >= MIN_WAV_BYTES) {
        fillWavHeader(header, dataBytes);
        xSemaphoreTake(xSPIlMutex, portMAX_DELAY);
        bool patched = lv_fs_seek(&file, 0, LV_FS_SEEK_SET) == LV_FS_RES_OK &&
                       lv_fs_write(&file, header, sizeof(header), nullptr) == LV_FS_RES_OK;
        lv_fs_close(&file);
        xSemaphoreGive(xSPIlMutex);
        success = patched;
    } else {
        xSemaphoreTake(xSPIlMutex, portMAX_DELAY);
        lv_fs_close(&file);
        xSemaphoreGive(xSPIlMutex);
    }
    digital_write(eAmp_Gain, 0);
    heap_caps_free(frame);
    heap_caps_free(preroll);
    if (!success) {
        SD.remove(INPUT_WAV);
        Serial.printf("VAD capture ended without uploadable speech bytes=%lu\n",
                      (unsigned long)dataBytes);
    } else {
        Serial.printf("VAD capture complete bytes=%lu duration=%lu ms\n",
                      (unsigned long)dataBytes,
                      (unsigned long)(dataBytes * 1000ULL / 64000ULL));
    }
    return success;
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
        if (checkWakeInterruption()) {
            cancelBridgeJob(id);
            Serial.println("Wake-word interruption requested during job");
            return "interrupted";
        }
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
                if (state == "ready" || state == "no_speech" || state == "conversation_end" || state == "error") {
                    if (state == "error") Serial.println(doc["error"].as<String>());
                    Serial.printf("Job terminal state=%s\n", state.c_str());
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

bool buildAudioEnvelope() {
    audioEnvelopeCount = 0;
    File f = SD.open(OUTPUT_WAV, FILE_READ);
    if (!f) return false;
    size_t bytes = f.size();
    if (bytes <= 44) {
        f.close();
        return false;
    }

    uint32_t totalFrames = static_cast<uint32_t>((bytes - 44) / 4);
    if (totalFrames == 0) {
        f.close();
        return false;
    }
    for (uint16_t i = 0; i < AUDIO_ENVELOPE_BINS; ++i) {
        audioEnvelopeSum[i] = 0;
        audioEnvelope[i] = 0;
    }

    f.seek(44);
    uint8_t buffer[4096];
    uint32_t frameIndex = 0;
    while (frameIndex < totalFrames) {
        size_t got = f.read(buffer, sizeof(buffer));
        if (got < 4) break;
        size_t usable = got - (got % 4);
        for (size_t offset = 0; offset < usable && frameIndex < totalFrames;
             offset += 4, ++frameIndex) {
            int32_t left = static_cast<int16_t>(
                static_cast<uint16_t>(buffer[offset]) |
                (static_cast<uint16_t>(buffer[offset + 1]) << 8));
            int32_t right = static_cast<int16_t>(
                static_cast<uint16_t>(buffer[offset + 2]) |
                (static_cast<uint16_t>(buffer[offset + 3]) << 8));
            if (left < 0) left = -left;
            if (right < 0) right = -right;
            uint16_t level = static_cast<uint16_t>((left + right) / 2);
            uint16_t bin = static_cast<uint16_t>(
                (static_cast<uint64_t>(frameIndex) * AUDIO_ENVELOPE_BINS) /
                totalFrames);
            if (bin >= AUDIO_ENVELOPE_BINS) bin = AUDIO_ENVELOPE_BINS - 1;
            audioEnvelopeSum[bin] += level;
        }
    }
    f.close();

    if (frameIndex == 0) return false;
    audioEnvelopeCount = static_cast<uint16_t>(
        min<uint32_t>(AUDIO_ENVELOPE_BINS,
                      (static_cast<uint64_t>(frameIndex) * AUDIO_ENVELOPE_BINS) /
                      totalFrames));
    if (audioEnvelopeCount == 0) audioEnvelopeCount = 1;
    for (uint16_t i = 0; i < audioEnvelopeCount; ++i) {
        uint32_t start = (static_cast<uint64_t>(i) * totalFrames) /
                         AUDIO_ENVELOPE_BINS;
        uint32_t end = (static_cast<uint64_t>(i + 1) * totalFrames) /
                       AUDIO_ENVELOPE_BINS;
        uint32_t framesInBin = max<uint32_t>(1, end - start);
        uint32_t average = audioEnvelopeSum[i] / framesInBin;
        audioEnvelope[i] = static_cast<uint8_t>(min<uint32_t>(
            255, (average * 255U) / 32767U));
    }
    return frameIndex == totalFrames;
}

uint8_t mouthFrameFromAmplitude(uint8_t amplitude) {
    if (amplitude < 4) return 0;
    if (amplitude < 24) return 1;
    if (amplitude < 82) return 2;
    if (amplitude < 170) return 3;
    return 4;
}

uint8_t mouthFrameFromAudio(uint8_t amplitude, uint8_t previousAmplitude) {
    uint8_t frame = mouthFrameFromAmplitude(amplitude);
    if (amplitude > previousAmplitude + 5 && frame < 4) ++frame;
    if (previousAmplitude > amplitude + 5 && frame > 0) --frame;
    return frame;
}

void settleMouthAfterPlayback(uint8_t mouthFrame) {
    if (mouthFrame >= 3) {
        face("speaking", "ARLO speaking", false, 2);
        delay(35);
    }
    if (mouthFrame >= 1) {
        face("speaking", "ARLO speaking", false, 1);
        delay(35);
    }
    if (mouthFrame != 0) {
        face("speaking", "ARLO speaking", false, 0);
        delay(45);
    }
}

bool playback(const String &activeJobId) {
    if (!SD.exists(OUTPUT_WAV)) return false;
    unsigned long expectedMs = outputWavDurationMs();
    unsigned long minimumPlaybackMs = expectedMs + 750UL;
    unsigned long timeoutMs = max(minimumPlaybackMs + 15000UL, 30000UL);
    Serial.printf("Playback WAV duration=%lu ms minimum=%lu ms\n",
                  expectedMs, minimumPlaybackMs);
    bool envelopeReady = buildAudioEnvelope();
    Serial.printf("Audio envelope bins=%u ready=%d\n",
                  audioEnvelopeCount, envelopeReady ? 1 : 0);
    face("speaking", "ARLO speaking", false, 0);
    music.playTFCardAudio("S:/arlo-reply.wav");

    // Follow the SDK playback task, but also enforce the WAV duration. The SDK
    // can report idle early, and starting recording immediately would make the
    // next listening turn compete with the tail of the current response.
    unsigned long began = millis();
    unsigned long nextMouthUpdate = began;
    unsigned long audioStartedAt = began;
    bool started = false;
    uint8_t mouthFrame = 0;
    while (millis() - began < timeoutMs) {
        if (checkWakeInterruption()) {
            Serial.println("Wake-word interruption requested during playback");
            cancelBridgeJob(activeJobId);
            music.stopPlayAudio();
            unsigned long stopStarted = millis();
            while (k10.getPlayMusicState() != 0 &&
                   millis() - stopStarted < 3000UL) {
                delay(20);
            }
            SD.remove(OUTPUT_WAV);
            face("listening", "Interrupted - listening");
            return true;
        }
        bool playing = k10.getPlayMusicState() != 0;
        if (playing && !started) {
            started = true;
            audioStartedAt = millis();
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
        if (now >= nextMouthUpdate) {
            uint8_t nextFrame = 0;
            if (started && expectedMs > 0 && envelopeReady &&
                audioEnvelopeCount > 0) {
                unsigned long audioElapsed = now - audioStartedAt;
                uint16_t index = static_cast<uint16_t>(min<uint64_t>(
                    audioEnvelopeCount - 1,
                    (static_cast<uint64_t>(audioElapsed) * audioEnvelopeCount) /
                    expectedMs));
                uint8_t previousAmplitude = index > 0
                    ? audioEnvelope[index - 1]
                    : audioEnvelope[index];
                nextFrame = mouthFrameFromAudio(audioEnvelope[index],
                                                 previousAmplitude);
            } else if (started && !envelopeReady) {
                nextFrame = static_cast<uint8_t>(((now - audioStartedAt) / 260UL) % 5);
            }
            mouthFrame = nextFrame;
            // This call also advances the speaking gaze every 900 ms, even
            // when the audio envelope stays in the same mouth band.
            face("speaking", "ARLO speaking", false, mouthFrame);
            nextMouthUpdate = now + 45;
        }
        delay(25);
    }
    if (millis() - began >= timeoutMs && k10.getPlayMusicState() != 0) {
        Serial.println("Playback timeout; stopping stuck audio task");
        music.stopPlayAudio();
    }
    settleMouthAfterPlayback(mouthFrame);
    return false;
}

bool voiceTurn(bool followup) {
    face("listening", followup ? "Listening..." : "Listening now");
    if (!recordUntilSpeechEnds()) {
        face("idle", "Say Jarvis to talk");
        return false;
    }
    face("thinking", "Sending audio...");
    String job = createJob();
    if (job.isEmpty()) {
        face("error", "Upload failed");
        delay(2000);
        return false;
    }
    String outcome = pollJob(job);
    if (outcome == "interrupted") {
        face("listening", "Interrupted - listening");
        return false;
    }
    if (outcome == "conversation_end") {
        face("thinking", "Saying goodbye...");
        if (!fetchAudio(job)) {
            face("error", "Goodbye audio failed");
            delay(2000);
            return false;
        }
        playback(job);
        face("idle", "Talk to you later");
        Serial.println("Conversation ended by explicit phrase");
        return true;
    }
    if (outcome == "no_speech") {
        face("idle", "Say Jarvis to talk");
        return false;
    }
    if (outcome != "ready") {
        face("error", "Bridge error - see USB");
        delay(2000);
        return false;
    }
    face("thinking", "Getting voice...");
    if (!fetchAudio(job)) {
        face("error", "Download failed");
        delay(2000);
        return false;
    }
    bool interrupted = playback(job);
    if (interrupted) return false;
    return false;
}

void conversation() {
    if (!sdReady || WiFi.status() != WL_CONNECTED) {
        face("error", "Check SD / Wi-Fi");
        return;
    }
    wakeCooldownUntil = millis() + 6000;
    armWakeInterruption();
    const unsigned long sessionStarted = millis();
    bool followup = false;
    while (millis() - sessionStarted < CONVERSATION_SESSION_MS) {
        bool ended = voiceTurn(followup);
        if (ended) break;
        // A silence turn or error ends the session. Successful turns leave the
        // face in speaking state, so the next iteration opens follow-up audio.
        if (faceState == "idle" || faceState == "error") break;
        followup = true;
        delay(450);
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
