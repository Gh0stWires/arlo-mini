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
#include <driver/i2s.h>
#include <freertos/ringbuf.h>
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
uint16_t jobSegmentsReady = 0;
uint16_t jobSegmentsTotal = 0;
bool jobStreamComplete = false;
bool jobPcmStreamReady = false;
bool jobPcmStreamComplete = false;

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
volatile bool pcmFaceRunning = false;
volatile uint8_t pcmFaceAmplitude = 0;
TaskHandle_t pcmFaceTaskHandle = nullptr;
constexpr bool PCM_MOUTH_ANIMATION = true;
uint8_t mouthFrameFromAudio(uint8_t amplitude, uint8_t previousAmplitude);

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

void drawRoundedRect(int x, int y, int width, int height, int radius,
                     uint32_t fill) {
    k10.canvas->canvasRectangle(x + radius, y, width - radius * 2,
                                height, fill, fill, true);
    k10.canvas->canvasRectangle(x, y + radius, width, height - radius * 2,
                                fill, fill, true);
    k10.canvas->canvasCircle(x + radius, y + radius, radius,
                             fill, fill, true);
    k10.canvas->canvasCircle(x + width - radius - 1, y + radius, radius,
                             fill, fill, true);
    k10.canvas->canvasCircle(x + radius, y + height - radius - 1, radius,
                             fill, fill, true);
    k10.canvas->canvasCircle(x + width - radius - 1,
                             y + height - radius - 1, radius,
                             fill, fill, true);
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
        drawRoundedRect(99, 195, 42, 25, 6, OUTLINE);
        drawRoundedRect(105, 201, 30, 13, 4, MOUTH_WINE);
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

void pcmFaceAnimationTask(void *) {
    uint8_t previousAmplitude = 0;
    while (pcmFaceRunning) {
        uint8_t amplitude = pcmFaceAmplitude;
        uint8_t frame = mouthFrameFromAudio(amplitude, previousAmplitude);
        face("speaking", "ARLO speaking", false, frame);
        previousAmplitude = amplitude;
        vTaskDelay(pdMS_TO_TICKS(90));
    }
    pcmFaceTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool startPcmFaceAnimation() {
    if (!PCM_MOUTH_ANIMATION || pcmFaceTaskHandle) return true;
    pcmFaceRunning = true;
    pcmFaceAmplitude = 0;
    BaseType_t result = xTaskCreate(
        pcmFaceAnimationTask, "pcm_face", 4096, nullptr, 0,
        &pcmFaceTaskHandle);
    if (result != pdPASS) {
        pcmFaceRunning = false;
        pcmFaceTaskHandle = nullptr;
        Serial.println("PCM face task creation failed");
        return false;
    }
    return true;
}

void stopPcmFaceAnimation() {
    if (!pcmFaceTaskHandle) return;
    pcmFaceRunning = false;
    while (pcmFaceTaskHandle) delay(1);
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
    Serial.printf("POST preflight wifi=%d rssi=%d ip=%s\n",
                  (int)WiFi.status(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
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
        http.setTimeout(60000);
        setCommonHeaders(http);
        http.addHeader("Content-Type", "audio/wav");
        int status = http.sendRequest("POST", &audio, audio.size());
        audio.close();
        String payload = status == 202 ? http.getString() : "";
        Serial.printf("POST /turn attempt=%u -> %d %s\n",
                      attempt, status, payload.c_str());
        http.end();
        if (status < 0) {
            Serial.printf("POST transport error=%d wifi=%d rssi=%d ip=%s\n",
                          status, (int)WiFi.status(), WiFi.RSSI(),
                          WiFi.localIP().toString().c_str());
            WiFi.disconnect(false);
            lastWiFiAttempt = 0;
            connectWifi();
            delay(1000);
        }
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
String pollJob(const String &id, uint16_t wantedSegment = 0,
                bool allowPcmStream = true) {
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
            DynamicJsonDocument doc(4096);
            String response = http.getString();
            DeserializationError parseError = deserializeJson(doc, response);
            if (parseError == DeserializationError::Ok) {
                String state = doc["state"].as<String>();
                jobSegmentsReady = doc["segments_ready"] | 0;
                jobSegmentsTotal = doc["segments_total"] | 0;
                jobStreamComplete = doc["stream_complete"] | false;
                jobPcmStreamReady = doc["pcm_stream_ready"] | false;
                jobPcmStreamComplete = doc["pcm_stream_complete"] | false;
                if (allowPcmStream && jobPcmStreamReady && wantedSegment == 0) {
                    http.end();
                    return "pcm_streaming";
                }
                if (jobSegmentsReady == 0 &&
                    (state == "ready" || state == "conversation_end")) {
                    // Keep compatibility with bridges that still expose one
                    // complete /audio file instead of sentence segments.
                    jobSegmentsReady = 1;
                    jobSegmentsTotal = 1;
                    jobStreamComplete = true;
                }
                if (jobSegmentsReady > wantedSegment) {
                    http.end();
                    return state;
                }
                if (state == "ready" || state == "no_speech" || state == "conversation_end" || state == "error") {
                    if (state == "error") Serial.println(doc["error"].as<String>());
                    Serial.printf("Job terminal state=%s\n", state.c_str());
                    http.end();
                    return state;
                }
                // Once playback has started, keep the speaking face while we
                // wait for the next streamed segment to become available.
                if (wantedSegment == 0) {
                    face("thinking", state + "...", false, 0);
                }
            } else {
                Serial.printf("GET /jobs parse failed: %s bytes=%u\n",
                              parseError.c_str(), (unsigned)response.length());
            }
        } else {
            Serial.printf("GET status -> %d\n", code);
        }
        http.end();
        delay(700);
    }
    return "error";
}

bool fetchAudio(const String &id, uint16_t segment = 0) {
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
        http.begin(String(BRIDGE_BASE_URL) + "/jobs/" + id +
                   "/audio?segment=" + String(segment));
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
    uint32_t peakAverage = 1;
    for (uint16_t i = 0; i < audioEnvelopeCount; ++i) {
        uint32_t start = (static_cast<uint64_t>(i) * totalFrames) /
                         AUDIO_ENVELOPE_BINS;
        uint32_t end = (static_cast<uint64_t>(i + 1) * totalFrames) /
                       AUDIO_ENVELOPE_BINS;
        uint32_t framesInBin = max<uint32_t>(1, end - start);
        peakAverage = max<uint32_t>(peakAverage,
                                    audioEnvelopeSum[i] / framesInBin);
    }
    for (uint16_t i = 0; i < audioEnvelopeCount; ++i) {
        uint32_t start = (static_cast<uint64_t>(i) * totalFrames) /
                         AUDIO_ENVELOPE_BINS;
        uint32_t end = (static_cast<uint64_t>(i + 1) * totalFrames) /
                       AUDIO_ENVELOPE_BINS;
        uint32_t framesInBin = max<uint32_t>(1, end - start);
        uint32_t average = audioEnvelopeSum[i] / framesInBin;
        audioEnvelope[i] = static_cast<uint8_t>(min<uint32_t>(
            255, (average * 255U) / peakAverage));
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

constexpr size_t PCM_RING_BYTES = 512 * 1024;
// Native CUDA chunks arrive in bursts. Keep roughly four seconds of decoded
// stereo PCM before opening I2S so chunk-level compute jitter is not audible.
constexpr size_t PCM_PREFILL_BYTES = 256 * 1024;
// Keep the PCM contract at 16 kHz. The prior 17.66 kHz calibration raised
// pitch noticeably; the remaining elapsed-time excess is producer pacing, not
// a reason to alter the audio pitch.
constexpr uint32_t PCM_I2S_PLAYBACK_RATE = 16000;
constexpr uint32_t PCM_I2S_IDLE_RATE = 16000;
constexpr unsigned long PCM_STREAM_TIMEOUT_MS = 60000UL;
// Optional one-shot isolation mode; production PCM uses direct I2S.
constexpr bool PCM_DIAGNOSTIC_USE_SDK_PLAYBACK = false;

struct PcmStreamContext {
    String jobId;
    RingbufHandle_t ring = nullptr;
    uint8_t *ringStorage = nullptr;
    StaticRingbuffer_t ringControl{};
    volatile bool finished = false;
    volatile bool failed = false;
    volatile size_t buffered = 0;
    volatile size_t downloaded = 0;
    volatile size_t minBuffered = SIZE_MAX;
    volatile size_t maxBuffered = 0;
    volatile uint32_t emptyReads = 0;
    volatile uint32_t blockedReceives = 0;
    volatile uint32_t maxReceiveWaitMs = 0;
    volatile uint32_t totalReceiveWaitMs = 0;
    volatile uint32_t i2sWrites = 0;
    volatile uint32_t i2sPartialWrites = 0;
    volatile uint32_t maxI2sWriteMs = 0;
    volatile uint32_t totalI2sWriteMs = 0;
    volatile size_t i2sRequestedBytes = 0;
    volatile size_t i2sWrittenBytes = 0;
    unsigned long playbackStartedMs = 0;
    unsigned long playbackFinishedMs = 0;
    int httpStatus = 0;
};

class PcmRingWriter : public Stream {
public:
    explicit PcmRingWriter(PcmStreamContext *context) : context_(context) {}

    size_t write(uint8_t value) override {
        return write(&value, 1);
    }

    size_t write(const uint8_t *data, size_t length) override {
        size_t consumed = 0;
        if (partialLength_ > 0) {
            size_t needed = 4 - partialLength_;
            size_t copied = min(needed, length);
            memcpy(partial_ + partialLength_, data, copied);
            partialLength_ += copied;
            consumed += copied;
            if (partialLength_ == 4) {
                if (!push(partial_, sizeof(partial_))) return 0;
                partialLength_ = 0;
            }
        }

        size_t remaining = length - consumed;
        size_t aligned = remaining - (remaining % 4);
        if (aligned > 0) {
            if (!push(data + consumed, aligned)) return 0;
            consumed += aligned;
        }
        size_t tail = length - consumed;
        if (tail > 0) {
            memcpy(partial_, data + consumed, tail);
            partialLength_ = tail;
            consumed += tail;
        }
        return consumed;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    bool hasPartialFrame() const { return partialLength_ != 0; }

private:
    bool push(const uint8_t *data, size_t length) {
        if (xRingbufferSend(context_->ring, data, length,
                            pdMS_TO_TICKS(1000)) != pdTRUE) {
            context_->failed = true;
            Serial.println("PCM stream ring buffer full/blocked");
            return false;
        }
        context_->buffered += length;
        context_->downloaded += length;
        if (context_->buffered > context_->maxBuffered) {
            context_->maxBuffered = context_->buffered;
        }
        return true;
    }

    PcmStreamContext *context_;
    uint8_t partial_[4] = {0, 0, 0, 0};
    size_t partialLength_ = 0;
};

void pcmDownloadTask(void *parameter) {
    PcmStreamContext *context = static_cast<PcmStreamContext *>(parameter);
    HTTPClient http;
    http.begin(String(BRIDGE_BASE_URL) + "/jobs/" + context->jobId + "/pcm");
    http.setReuse(false);
    http.useHTTP10(true);
    http.setConnectTimeout(10000);
    http.setTimeout(5000);
    setCommonHeaders(http);
    int status = http.GET();
    context->httpStatus = status;
    if (status != 200) {
        context->failed = true;
        Serial.printf("PCM stream HTTP status=%d\n", status);
        http.end();
        context->finished = true;
        vTaskDelete(nullptr);
        return;
    }

    PcmRingWriter writer(context);
    int decodedBytes = http.writeToStream(&writer);
    if (decodedBytes < 0 || writer.hasPartialFrame()) {
        context->failed = true;
        Serial.printf("PCM stream decode failed result=%d partial=%d\n",
                      decodedBytes, writer.hasPartialFrame() ? 1 : 0);
    }
    http.end();
    context->finished = true;
    Serial.printf("PCM stream download bytes=%u failed=%d\n",
                  (unsigned)context->downloaded, context->failed ? 1 : 0);
    vTaskDelete(nullptr);
}

uint8_t pcmAmplitude(const uint8_t *data, size_t bytes) {
    if (!data || bytes < 4) return 0;
    uint64_t total = 0;
    size_t frames = bytes / 4;
    for (size_t offset = 0; offset + 3 < bytes; offset += 4) {
        int32_t left = static_cast<int16_t>(
            static_cast<uint16_t>(data[offset]) |
            (static_cast<uint16_t>(data[offset + 1]) << 8));
        int32_t right = static_cast<int16_t>(
            static_cast<uint16_t>(data[offset + 2]) |
            (static_cast<uint16_t>(data[offset + 3]) << 8));
        if (left < 0) left = -left;
        if (right < 0) right = -right;
        total += static_cast<uint32_t>((left + right) / 2);
    }
    return static_cast<uint8_t>(min<uint64_t>(255, total / max<size_t>(1, frames) / 128));
}

bool writeFileFully(File &file, const uint8_t *data, size_t length) {
    size_t writtenTotal = 0;
    while (writtenTotal < length) {
        size_t written = file.write(data + writtenTotal, length - writtenTotal);
        if (written == 0) return false;
        writtenTotal += written;
    }
    return true;
}

void playback();

bool playPcmStream(const String &id, size_t &playedBytes) {
    playedBytes = 0;
    PcmStreamContext context;
    context.jobId = id;
    context.ringStorage = static_cast<uint8_t *>(heap_caps_malloc(
        PCM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!context.ringStorage) {
        Serial.println("PCM stream PSRAM ring allocation failed");
        return false;
    }
    context.ring = xRingbufferCreateStatic(
        PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF, context.ringStorage,
        &context.ringControl);
    if (!context.ring) {
        heap_caps_free(context.ringStorage);
        context.ringStorage = nullptr;
        Serial.println("PCM stream ring creation failed");
        return false;
    }
    BaseType_t taskResult = xTaskCreate(
        pcmDownloadTask, "pcm_download", 16384, &context, 3, nullptr);
    if (taskResult != pdPASS) {
        vRingbufferDelete(context.ring);
        heap_caps_free(context.ringStorage);
        context.ring = nullptr;
        context.ringStorage = nullptr;
        Serial.println("PCM stream task creation failed");
        return false;
    }

    unsigned long prefillStarted = millis();
    while (!context.finished && context.buffered < PCM_PREFILL_BYTES &&
           millis() - prefillStarted < 15000UL) {
        delay(5);
    }
    if (context.buffered == 0 && context.finished) {
        Serial.printf("PCM stream had no audio status=%d\n", context.httpStatus);
        vRingbufferDelete(context.ring);
        heap_caps_free(context.ringStorage);
        context.ring = nullptr;
        context.ringStorage = nullptr;
        return false;
    }

    Serial.printf("PCM playback start buffered=%u downloaded=%u\n",
                  (unsigned)context.buffered, (unsigned)context.downloaded);
    context.playbackStartedMs = millis();
    context.minBuffered = context.buffered;
    face("speaking", "ARLO speaking", false, 0);
    startPcmFaceAnimation();
    File diagnosticWav;
    uint32_t diagnosticBytes = 0;
    if (PCM_DIAGNOSTIC_USE_SDK_PLAYBACK) {
        SD.remove(OUTPUT_WAV);
        diagnosticWav = SD.open(OUTPUT_WAV, FILE_WRITE);
        if (!diagnosticWav) {
            Serial.println("PCM diagnostic WAV open failed");
            context.failed = true;
        } else {
            uint8_t header[44];
            fillWavHeader(header, 0);
            if (!writeFileFully(diagnosticWav, header, sizeof(header))) {
                Serial.println("PCM diagnostic WAV header write failed");
                context.failed = true;
            }
        }
    } else {
        xSemaphoreTake(xI2SMutex, portMAX_DELAY);
        esp_err_t clockResult = i2s_set_sample_rates(
            I2S_NUM_0, PCM_I2S_PLAYBACK_RATE);
        Serial.printf("PCM I2S playback clock requested=%u actual=%u result=%d\n",
                      (unsigned)PCM_I2S_PLAYBACK_RATE,
                      (unsigned)i2s_get_clk(I2S_NUM_0), (int)clockResult);
        i2s_zero_dma_buffer(I2S_NUM_0);
    }
    unsigned long lastData = millis();
    unsigned long nextMouthUpdate = millis();
    uint8_t mouthFrame = 0;
    uint8_t previousAmplitude = 0;
    while (!context.finished || context.buffered > 0) {
        size_t itemSize = 0;
        unsigned long receiveStarted = millis();
        uint8_t *item = static_cast<uint8_t *>(xRingbufferReceiveUpTo(
            context.ring, &itemSize, pdMS_TO_TICKS(100), 8192));
        unsigned long receiveWait = millis() - receiveStarted;
        if (receiveWait > 0) {
            context.totalReceiveWaitMs += receiveWait;
            if (receiveWait > context.maxReceiveWaitMs) {
                context.maxReceiveWaitMs = receiveWait;
            }
            if (receiveWait >= 2) context.blockedReceives++;
        }
        if (!item || itemSize == 0) {
            context.emptyReads++;
            if (context.finished && context.buffered == 0) break;
            if (millis() - lastData > 10000UL) {
                context.failed = true;
                Serial.println("PCM stream playback underrun/timeout");
                break;
            }
            continue;
        }
        size_t written = 0;
        esp_err_t result = ESP_OK;
        if (PCM_DIAGNOSTIC_USE_SDK_PLAYBACK) {
            xSemaphoreTake(xSPIlMutex, portMAX_DELAY);
            bool writeOk = writeFileFully(diagnosticWav, item, itemSize);
            xSemaphoreGive(xSPIlMutex);
            written = writeOk ? itemSize : 0;
            diagnosticBytes += written;
        } else {
            unsigned long i2sStarted = millis();
            result = i2s_write(I2S_NUM_0, item, itemSize, &written,
                               pdMS_TO_TICKS(2000));
            unsigned long i2sWait = millis() - i2sStarted;
            context.totalI2sWriteMs += i2sWait;
            if (i2sWait > context.maxI2sWriteMs) {
                context.maxI2sWriteMs = i2sWait;
            }
        }
        uint8_t amplitude = pcmAmplitude(item, itemSize);
        vRingbufferReturnItem(context.ring, item);
        context.buffered -= itemSize;
        if (context.buffered < context.minBuffered) {
            context.minBuffered = context.buffered;
        }
        context.i2sWrites++;
        context.i2sRequestedBytes += itemSize;
        context.i2sWrittenBytes += written;
        if (written != itemSize) context.i2sPartialWrites++;
        if (!PCM_DIAGNOSTIC_USE_SDK_PLAYBACK &&
            (result != ESP_OK || written != itemSize)) {
            context.failed = true;
            Serial.printf("PCM stream I2S write failed err=%d wrote=%u/%u\n",
                          (int)result, (unsigned)written, (unsigned)itemSize);
            while (!context.finished) {
                size_t discardedSize = 0;
                uint8_t *discarded = static_cast<uint8_t *>(xRingbufferReceiveUpTo(
                    context.ring, &discardedSize, pdMS_TO_TICKS(100), 8192));
                if (discarded) {
                    vRingbufferReturnItem(context.ring, discarded);
                    context.buffered -= discardedSize;
                }
            }
            break;
        }
        playedBytes += written;
        lastData = millis();
        if (PCM_MOUTH_ANIMATION) {
            pcmFaceAmplitude = amplitude;
        }
    }
    while (!context.finished) delay(10);
    stopPcmFaceAnimation();
    context.playbackFinishedMs = millis();
    unsigned long elapsed = context.playbackFinishedMs - context.playbackStartedMs;
    unsigned long expected = static_cast<unsigned long>(
        (static_cast<uint64_t>(playedBytes) * 1000ULL) / 64000ULL);
    Serial.printf("PCM playback stats played=%u expected_ms=%lu elapsed_ms=%lu min_buffer=%u max_buffer=%u empty_reads=%u blocked_receives=%u receive_wait_ms=%u max_receive_wait_ms=%u writes=%u partial=%u requested=%u written=%u i2s_wait_ms=%u max_i2s_wait_ms=%u\n",
                  (unsigned)playedBytes, expected, elapsed,
                  (unsigned)context.minBuffered, (unsigned)context.maxBuffered,
                  (unsigned)context.emptyReads, (unsigned)context.blockedReceives,
                  (unsigned)context.totalReceiveWaitMs,
                  (unsigned)context.maxReceiveWaitMs,
                  (unsigned)context.i2sWrites,
                  (unsigned)context.i2sPartialWrites,
                  (unsigned)context.i2sRequestedBytes,
                  (unsigned)context.i2sWrittenBytes,
                  (unsigned)context.totalI2sWriteMs,
                  (unsigned)context.maxI2sWriteMs);
    if (PCM_DIAGNOSTIC_USE_SDK_PLAYBACK && diagnosticWav) {
        uint8_t header[44];
        fillWavHeader(header, diagnosticBytes);
        xSemaphoreTake(xSPIlMutex, portMAX_DELAY);
        diagnosticWav.seek(0);
        bool headerOk = writeFileFully(diagnosticWav, header, sizeof(header));
        diagnosticWav.close();
        xSemaphoreGive(xSPIlMutex);
        if (!headerOk) {
            context.failed = true;
            Serial.println("PCM diagnostic WAV final header write failed");
        }
        Serial.printf("PCM diagnostic WAV ready bytes=%u; using SDK playback\n",
                      (unsigned)diagnosticBytes);
        playback();
    }
    bool success = !context.failed && playedBytes > 0 && context.downloaded > 0;
    if (!PCM_DIAGNOSTIC_USE_SDK_PLAYBACK) {
        esp_err_t idleClockResult = i2s_set_sample_rates(
            I2S_NUM_0, PCM_I2S_IDLE_RATE);
        Serial.printf("PCM I2S idle clock requested=%u actual=%u result=%d\n",
                      (unsigned)PCM_I2S_IDLE_RATE,
                      (unsigned)i2s_get_clk(I2S_NUM_0), (int)idleClockResult);
        xSemaphoreGive(xI2SMutex);
    }
    vRingbufferDelete(context.ring);
    heap_caps_free(context.ringStorage);
    context.ring = nullptr;
    context.ringStorage = nullptr;
    if (success) {
        settleMouthAfterPlayback(mouthFrame);
        Serial.printf("PCM stream playback complete bytes=%u\n", (unsigned)playedBytes);
    } else {
        Serial.printf("PCM stream playback failed bytes=%u downloaded=%u\n",
                      (unsigned)playedBytes, (unsigned)context.downloaded);
    }
    return success;
}
void playback() {
    if (!SD.exists(OUTPUT_WAV)) return;
    unsigned long expectedMs = outputWavDurationMs();
    unsigned long minimumPlaybackMs = expectedMs + 750UL;
    Serial.printf("Playback WAV duration=%lu ms minimum=%lu ms\n",
                  expectedMs, minimumPlaybackMs);
    bool envelopeReady = buildAudioEnvelope();
    Serial.printf("Audio envelope bins=%u ready=%d\n",
                  audioEnvelopeCount, envelopeReady ? 1 : 0);
    face("speaking", "ARLO speaking", false, 0);
    music.playTFCardAudio("S:/arlo-reply.wav");

    // The K10 SDK's getPlayMusicState() can block while its playback task owns
    // the audio/SPI resources. Use the WAV duration as the completion signal
    // instead of polling that state from the main loop. This keeps the face
    // responsive and guarantees that the conversation state can advance.
    unsigned long began = millis();
    unsigned long nextMouthUpdate = began;
    uint8_t mouthFrame = 0;
    while (millis() - began < minimumPlaybackMs) {
        unsigned long now = millis();
        if (now >= nextMouthUpdate) {
            uint8_t nextFrame = 0;
            if (expectedMs > 0 && envelopeReady && audioEnvelopeCount > 0) {
                uint16_t index = static_cast<uint16_t>(min<uint64_t>(
                    audioEnvelopeCount - 1,
                    (static_cast<uint64_t>(now - began) * audioEnvelopeCount) /
                    expectedMs));
                uint8_t previousAmplitude = index > 0
                    ? audioEnvelope[index - 1]
                    : audioEnvelope[index];
                nextFrame = mouthFrameFromAudio(audioEnvelope[index],
                                                 previousAmplitude);
            }
            mouthFrame = nextFrame;
            face("speaking", "ARLO speaking", false, mouthFrame);
            nextMouthUpdate = now + 90;
        }
        delay(25);
    }
    music.stopPlayAudio();
    Serial.printf("Playback duration wait complete elapsed=%lu ms\n",
                  millis() - began);
    settleMouthAfterPlayback(mouthFrame);
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
    jobSegmentsReady = 0;
    jobSegmentsTotal = 0;
    jobStreamComplete = false;
    jobPcmStreamReady = false;
    jobPcmStreamComplete = false;
    String outcome = pollJob(job, 0);
    if (outcome == "interrupted") {
        face("listening", "Interrupted - listening");
        return false;
    }
    if (outcome == "no_speech") {
        face("idle", "Say Jarvis to talk");
        return false;
    }
    if (outcome != "pcm_streaming" && outcome != "streaming" &&
        outcome != "ready" && outcome != "conversation_end") {
        face("error", "Bridge error - see USB");
        delay(2000);
        return false;
    }
    bool conversationEnded = outcome == "conversation_end";
    uint16_t segment = 0;
    if (outcome == "pcm_streaming") {
        size_t playedBytes = 0;
        bool streamed = playPcmStream(job, playedBytes);
        if (streamed) {
            String terminal = pollJob(job, 0);
            if (terminal == "interrupted") {
                face("listening", "Interrupted - listening");
                return false;
            }
            conversationEnded = terminal == "conversation_end";
            if (conversationEnded) {
                face("idle", "Talk to you later");
                Serial.println("Conversation ended by explicit phrase");
                return true;
            }
            return false;
        }
        if (playedBytes > 0) {
            face("error", "PCM stream interrupted");
            delay(2000);
            return false;
        }
        // No PCM reached I2S. Continue through the complete-WAV fallback.
        outcome = pollJob(job, 0, false);
    }
    if (outcome == "interrupted") {
        face("listening", "Interrupted - listening");
        return false;
    }

    while (true) {
        if (segment >= jobSegmentsReady) {
            outcome = pollJob(job, segment);
            if (outcome == "interrupted") {
                face("listening", "Interrupted - listening");
                return false;
            }
            if (outcome == "no_speech" || outcome == "error") {
                face("error", "Bridge error - see USB");
                delay(2000);
                return false;
            }
            conversationEnded = conversationEnded || outcome == "conversation_end";
        }
        if (segment >= jobSegmentsReady) {
            face("error", "No sentence audio");
            delay(2000);
            return false;
        }
        // Keep the speaking face between streamed segments. Switching to the
        // thinking face here causes a visible flash before the next clip.
        if (segment == 0) {
            face("thinking", "Getting voice...");
        }
        if (!fetchAudio(job, segment)) {
            face("error", "Download failed");
            delay(2000);
            return false;
        }
        playback();
        ++segment;
        if (jobStreamComplete && segment >= jobSegmentsTotal) break;
    }
    if (conversationEnded) {
        face("idle", "Talk to you later");
        Serial.println("Conversation ended by explicit phrase");
        return true;
    }
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
