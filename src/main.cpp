
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>
#include "esp_sdr.h"
#include "spectrum.h"
#include "touch_input.h"
#include "splash_image.h"

// Fixed receiver gain index (clamped to the hardware maximum at start-up).
constexpr uint8_t kReceiverGain = 30;

namespace color {
constexpr uint16_t BG = 0x0000, DIM = 0x0333, MID = 0x03BF;
constexpr uint16_t BRIGHT = 0x07FF, ERROR = 0xF800;
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t TRACE = rgb(170, 230, 120);
const uint16_t FILL = rgb(45, 85, 35);
}

// Title sits at y=0, status line right below it; the plot starts immediately after.
constexpr int kTitleY = 0, kStatusY = 16, kPlotTop = 26, kPlotLeft = 6;
// Vertical scale of the spectrum trace (dBFS). Narrower span = more vertical
// resolution. These are the start-up values; the scale then follows the signal.
constexpr int kPlotMaxDbfs = -10, kPlotMinDbfs = -90;
constexpr int kFrequencyStripHeight = 12;
// Ring capacity covers the full-screen waterfall; the shared layout uses fewer rows.
constexpr int kWaterfallRowHeight = 2, kMaxWaterfallRows = 100, kNormalWaterfallRows = 42;
constexpr int kBacklightChannel = 0, kBootPin = 0;
// Spectrum trace style, cycled by tapping the plot, BOOT, or serial "mode N".
enum TraceMode { kTraceDots, kTraceLine, kTraceFilled, kTraceGradient, kTraceModeCount };
static const char* const kTraceModeNames[kTraceModeCount] = {"dots", "line", "filled", "gradient"};
// Waterfall colour palette, cycled by tapping the lower half of the screen.
enum WaterfallStyle { kWfClassic, kWfThermal, kWfPhosphor, kWfGray, kWfRainbow, kWfStyleCount };
static const char* const kWaterfallStyleNames[kWfStyleCount] =
    {"classic", "thermal", "phosphor", "gray", "rainbow"};
static int waterfallStyle = kWfClassic;
constexpr int kMaxPlotHeight = 320;
// Gradient trace colour per plot row (index 0 = top row); built once in setup().
static uint16_t rowGradient[kMaxPlotHeight];
static int traceMode = kTraceLine;
TFT_eSPI tft;
QueueHandle_t frameQueue;
static volatile bool burstFilterEnabled = true;
static uint8_t appliedReceiverGain = kReceiverGain;
static int plotHeight, waterfallTop, waterfallRows;
// Screen layout: both parts, or one of them over the whole page (tick boxes).
enum ScreenLayout { kLayoutBoth, kLayoutSpectrum, kLayoutWaterfall, kLayoutCount };
static const char* const kLayoutNames[kLayoutCount] =
    {"spectrum + waterfall", "full-screen spectrum", "full-screen waterfall"};
static int screenLayout = kLayoutBoth;
static uint8_t waterfall[kMaxWaterfallRows][kSpectrumBins] = {};
static int waterfallHead = 0;
static uint8_t smooth[kSpectrumBins] = {};
static bool smoothPrimed = false;

static uint16_t waterfallColor(uint8_t value) {
    static const uint8_t palettes[kWfStyleCount][6][3] = {
        // classic: navy -> blue -> gold -> red
        {{6,9,26}, {27,42,90}, {45,79,145}, {79,139,182}, {212,180,61}, {227,74,42}},
        // thermal: black -> purple -> red -> orange -> yellow -> white
        {{5,0,15}, {70,10,110}, {190,35,70}, {245,110,20}, {250,210,60}, {255,255,230}},
        // phosphor: dark green CRT
        {{0,6,2}, {0,40,10}, {10,100,25}, {40,170,50}, {130,230,90}, {220,255,200}},
        // gray
        {{4,4,6}, {40,40,48}, {90,90,100}, {150,150,158}, {210,210,214}, {255,255,255}},
        // rainbow: violet -> blue -> cyan -> green -> yellow -> red
        {{10,0,40}, {0,40,200}, {0,190,200}, {40,200,40}, {240,220,30}, {240,40,30}},
    };
    const uint8_t (*stops)[3] = palettes[waterfallStyle];
    const int above = (int)value - 50;
    if (above <= 0) return color::rgb(stops[0][0], stops[0][1], stops[0][2]);
    const float position = above * 5.0f / 255.0f;
    int index = (int)position;
    if (index > 4) index = 4;
    const float fraction = position - index;
    int channel[3];
    for (int c = 0; c < 3; ++c)
        channel[c] = stops[index][c]
            + (int)(((int)stops[index + 1][c] - stops[index][c]) * fraction);
    return color::rgb(channel[0], channel[1], channel[2]);
}

// Colour depends on screen row, not on level: dark green at the bottom, through
// green and yellow, to red at the top (the plateau sits near the top).
static void buildRowGradient() {
    static const uint8_t stops[4][3] = {
        {0, 60, 10}, {25, 190, 35}, {235, 215, 40}, {240, 45, 30}
    };
    for (int row = 0; row < plotHeight && row < kMaxPlotHeight; ++row) {
        const float t = plotHeight > 1 ? 1.0f - (float)row / (plotHeight - 1) : 1.0f;
        const float position = t * 3.0f;
        int index = (int)position;
        if (index > 2) index = 2;
        const float fraction = position - index;
        int channel[3];
        for (int c = 0; c < 3; ++c)
            channel[c] = stops[index][c]
                + (int)(((int)stops[index + 1][c] - stops[index][c]) * fraction);
        rowGradient[row] = color::rgb(channel[0], channel[1], channel[2]);
    }
}

// ---- Auto-scale: detect a transmission plateau and put it near the plot top ----
constexpr float kPlateauFraction = 0.95f;    // plateau height as share of the plot
constexpr int kDetectMarginDb = 10;          // run must be this far above the floor
constexpr int kDetectMinBins = 8;            // narrower runs (spikes) are ignored
constexpr int kDetectFrames = 3;             // consecutive frames before locking
constexpr float kScaleSlewDbPerSec = 5.0f;
constexpr uint32_t kScaleHoldMs = 10000;     // keep the scale this long after loss
constexpr float kScaleTopMinDbfs = -60.0f, kScaleTopMaxDbfs = 0.0f;
constexpr float kFloorMarginDb = 6.0f;      // scale bottom sits this far below the floor
constexpr float kScaleBottomMinDbfs = -100.0f, kScaleBottomMaxDbfs = -65.0f;
constexpr float kMinScaleSpanDb = 20.0f;
static float plotTopDbfs = kPlotMaxDbfs;
static float plotBottomDbfs = kPlotMinDbfs;

// Median of 8-bit codes via histogram (no sorting, no allocation).
static int medianCode(const uint8_t* values, int count) {
    uint16_t histogram[256] = {};
    for (int i = 0; i < count; ++i) ++histogram[values[i]];
    int seen = 0;
    for (int code = 0; code < 256; ++code) {
        seen += histogram[code];
        if (seen * 2 >= count) return code;
    }
    return 255;
}

static void updateAutoScale(const uint8_t* values, int lo, int hi) {
    static uint32_t lastMs = 0, lastDetectMs = 0;
    static int hits = 0;
    static bool locked = false, everDetected = false;
    static float plateauDb = 0;
    const uint32_t now = millis();
    float dt = lastMs ? (now - lastMs) / 1000.0f : 0.0f;
    if (dt > 0.2f) dt = 0.2f;
    lastMs = now;

    const int count = hi - lo + 1;
    const int floorCode = medianCode(values + lo, count);
    static float floorDb = 0;
    static bool floorKnown = false;
    const float measuredFloor = -100.0f + floorCode * 100.0f / 255.0f;
    floorDb = floorKnown ? floorDb + 0.1f * (measuredFloor - floorDb) : measuredFloor;
    floorKnown = true;
    const int threshold = floorCode + kDetectMarginDb * 255 / 100;
    int bestStart = 0, bestLength = 0, runStart = -1;
    for (int i = lo; i <= hi + 1; ++i) {
        const bool above = i <= hi && values[i] >= threshold;
        if (above && runStart < 0) runStart = i;
        if (!above && runStart >= 0) {
            if (i - runStart > bestLength) { bestStart = runStart; bestLength = i - runStart; }
            runStart = -1;
        }
    }

    if (bestLength >= kDetectMinBins) {
        // Median of the middle 60% of the run: skips the sloping edges.
        const int edge = bestLength / 5;
        const int plateauCode = medianCode(values + bestStart + edge, bestLength - 2 * edge);
        const float measured = -100.0f + plateauCode * 100.0f / 255.0f;
        plateauDb = hits ? plateauDb + 0.2f * (measured - plateauDb) : measured;
        ++hits;
        lastDetectMs = now;
        everDetected = true;
    } else hits = 0;

    const bool active = hits >= kDetectFrames;
    if (active != locked) {
        locked = active;
        Serial.printf("[scale] transmission %s (floor %.0f dBFS, plateau %.0f dBFS, %d bins)\n",
                      locked ? "detected" : "lost",
                      -100.0f + floorCode * 100.0f / 255.0f, plateauDb, bestLength);
    }

    // Bottom follows the noise floor so no space is wasted below it.
    float bottomTarget = floorDb - kFloorMarginDb;
    if (bottomTarget < kScaleBottomMinDbfs) bottomTarget = kScaleBottomMinDbfs;
    if (bottomTarget > kScaleBottomMaxDbfs) bottomTarget = kScaleBottomMaxDbfs;

    float target = plotTopDbfs;  // hold by default
    if (active) {
        target = plotBottomDbfs + (plateauDb - plotBottomDbfs) / kPlateauFraction;
        if (target < kScaleTopMinDbfs) target = kScaleTopMinDbfs;
        if (target > kScaleTopMaxDbfs) target = kScaleTopMaxDbfs;
    } else if (!everDetected || now - lastDetectMs >= kScaleHoldMs) {
        target = kPlotMaxDbfs;
    }
    if (target < bottomTarget + kMinScaleSpanDb) target = bottomTarget + kMinScaleSpanDb;

    const float step = kScaleSlewDbPerSec * dt;
    if (target > plotTopDbfs + step) plotTopDbfs += step;
    else if (target < plotTopDbfs - step) plotTopDbfs -= step;
    else plotTopDbfs = target;
    if (bottomTarget > plotBottomDbfs + step) plotBottomDbfs += step;
    else if (bottomTarget < plotBottomDbfs - step) plotBottomDbfs -= step;
    else plotBottomDbfs = bottomTarget;
}

// Draw once: this strip is outside the trace and waterfall repaint regions.
static void drawFrequencyLabels() {
    const int right = tft.width() - 6, width = right - kPlotLeft;
    const int baseline = waterfallTop - 5;
    tft.setTextColor(color::BRIGHT, color::BG);
    tft.setTextDatum(BL_DATUM);
    tft.drawString("2401.0", kPlotLeft + 2, baseline, 1);
    tft.setTextDatum(BC_DATUM);
    tft.drawString("2405.5 MHz", kPlotLeft + width / 2, baseline, 1);
    tft.setTextDatum(BR_DATUM);
    tft.drawString("2410.0", right - 2, baseline, 1);
}

// Tick boxes in the top corners: left = full spectrum, right = full waterfall.
static void drawCheckbox(int x, bool ticked) {
    tft.drawRect(x, 1, 14, 14, color::MID);
    tft.fillRect(x + 2, 3, 10, 10, ticked ? color::MID : color::BG);
}

static void drawCheckboxes() {
    drawCheckbox(2, screenLayout == kLayoutSpectrum);
    drawCheckbox(tft.width() - 16, screenLayout == kLayoutWaterfall);
}

// Lays out the screen for the current mode and redraws the static elements.
static void applyLayout() {
    const int available = tft.height() - kPlotTop - 5;
    if (screenLayout == kLayoutWaterfall) {
        plotHeight = 0;
        waterfallTop = kPlotTop + kFrequencyStripHeight + 4;
        waterfallRows = (tft.height() - 5 - waterfallTop) / kWaterfallRowHeight;
        if (waterfallRows > kMaxWaterfallRows) waterfallRows = kMaxWaterfallRows;
    } else {
        if (screenLayout == kLayoutSpectrum) {
            waterfallRows = 0;
        } else {
            const int preferredWaterfall = available - available * 60 / 100;
            waterfallRows = preferredWaterfall / kWaterfallRowHeight;
            if (waterfallRows > kNormalWaterfallRows) waterfallRows = kNormalWaterfallRows;
        }
        plotHeight = available - waterfallRows * kWaterfallRowHeight - kFrequencyStripHeight;
        waterfallTop = kPlotTop + plotHeight + kFrequencyStripHeight + 4;
    }
    buildRowGradient();
    tft.fillRect(0, kPlotTop, tft.width(), tft.height() - kPlotTop, color::BG);
    tft.drawFastHLine(kPlotLeft, kPlotTop + plotHeight,
                      tft.width() - kPlotLeft - 6, color::DIM);
    drawFrequencyLabels();
    if (waterfallRows > 0)
        tft.drawRect(5, waterfallTop - 1, tft.width() - 10,
                     waterfallRows * kWaterfallRowHeight + 2, color::DIM);
    tft.fillRect(0, 0, 18, kStatusY - 1, color::BG);
    tft.fillRect(tft.width() - 18, 0, 18, kStatusY - 1, color::BG);
    drawCheckboxes();
}

static void drawSpectrumTrace(int width, int right) {
    const float span = plotTopDbfs - plotBottomDbfs;
    int prevY = kPlotTop;
    for (int pixel = 0; pixel < width; ++pixel) {
        const int bin = spectrumBinForPixel(pixel, width);
        const int dbfs = -100 + smooth[bin] * 100 / 255;
        int y = kPlotTop + (int)lroundf((plotTopDbfs - dbfs) * (plotHeight - 1) / span);
        if (y < kPlotTop) y = kPlotTop;
        if (y > kPlotTop + plotHeight - 1) y = kPlotTop + plotHeight - 1;
        const int x = kPlotLeft + pixel;
        tft.drawFastVLine(x, kPlotTop, plotHeight, color::BG);
        if (traceMode == kTraceGradient && plotHeight <= kMaxPlotHeight) {
            tft.startWrite();
            tft.setAddrWindow(x, y, 1, kPlotTop + plotHeight - y);
            tft.pushColors(rowGradient + (y - kPlotTop), kPlotTop + plotHeight - y);
            tft.endWrite();
        } else {
            if (traceMode == kTraceFilled)
                tft.drawFastVLine(x, y, kPlotTop + plotHeight - y, color::FILL);
            // Each segment only touches columns x-1 and x, so the per-column
            // clear above never erases it.
            if (traceMode != kTraceDots && pixel > 0) tft.drawLine(x - 1, prevY, x, y, color::TRACE);
            else tft.drawPixel(x, y, color::TRACE);
        }
        prevY = y;
    }
    // Grid stays on multiples of 10 dBFS, so it slides as the scale adapts.
    for (int db = (int)floorf(plotTopDbfs / 10.0f) * 10; db >= plotBottomDbfs; db -= 10) {
        const int y = kPlotTop + (int)lroundf((plotTopDbfs - db) * (plotHeight - 1) / span);
        for (int x = kPlotLeft; x < right; x += 6) {
            const int dash = x + 3 > right ? right - x : 3;
            tft.drawFastHLine(x, y, dash, color::rgb(30,40,55));
        }
    }
}

static void drawFrame(const SpectrumFrame& frame) {
    const int right = tft.width() - 6, width = right - kPlotLeft;
    static int lastGain = -1;
    static bool lastFilter = false;
    if (!frame.valid || frame.gain != lastGain || frame.burstFiltered != lastFilter) {
        smoothPrimed = false;
        memset(waterfall, 0, sizeof(waterfall));
    }
    lastGain = frame.gain;
    lastFilter = frame.burstFiltered;
    char status[64];
    if (!frame.valid) snprintf(status, sizeof(status), "%s", frame.error);
    else if (frame.clipped) snprintf(status, sizeof(status), "CLIPPING (gain %u)", frame.gain);
    else snprintf(status, sizeof(status), "31.25 kHz/bin");
    static char lastStatus[64] = {};
    static uint16_t lastStatusColor = 0;
    const uint16_t statusColor = frame.valid && !frame.clipped ? color::MID : color::ERROR;
    if (strcmp(status, lastStatus) || statusColor != lastStatusColor) {
        tft.fillRect(0, kStatusY, tft.width(), 9, color::BG);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(statusColor, color::BG);
        tft.drawString(status, tft.width() / 2, kStatusY, 1);
        snprintf(lastStatus, sizeof(lastStatus), "%s", status);
        lastStatusColor = statusColor;
    }

    for (int i = 0; i < kSpectrumBins; ++i)
        smooth[i] = smoothPrimed ? (smooth[i] * 2 + frame.bins[i]) / 3 : frame.bins[i];
    smoothPrimed = frame.valid;
    if (frame.valid)
        updateAutoScale(smooth, spectrumBinForPixel(0, width),
                        spectrumBinForPixel(width - 1, width));
    if (plotHeight > 0) drawSpectrumTrace(width, right);

    memcpy(waterfall[waterfallHead], frame.bins, sizeof(frame.bins));
    waterfallHead = (waterfallHead + 1) % kMaxWaterfallRows;
    static uint16_t line[320];
    for (int row = 0; row < waterfallRows; ++row) {
        // Head points to the next write slot; newest row is just before it.
        const int ring = (waterfallHead - 1 - row + kMaxWaterfallRows) % kMaxWaterfallRows;
        uint16_t colors[kSpectrumBins];
        for (int bin = 0; bin < kSpectrumBins; ++bin)
            colors[bin] = waterfallColor(waterfall[ring][bin]);
        for (int pixel = 0; pixel < width; ++pixel)
            line[pixel] = colors[spectrumBinForPixel(pixel, width)];
        tft.startWrite();
        tft.setAddrWindow(kPlotLeft, waterfallTop + row * kWaterfallRowHeight,
                          width, kWaterfallRowHeight);
        for (int repeat = 0; repeat < kWaterfallRowHeight; ++repeat)
            tft.pushColors(line, width);
        tft.endWrite();
    }
}

static void receiverTask(void*) {
    static SpectrumProcessor processor;
    static uint32_t samples[kSpectrumBins];
    int appliedGain = -1;
    bool tuned = false;
    for (;;) {
        SpectrumFrame frame;
        processor.setBurstFilterEnabled(burstFilterEnabled);
        bool configured = espSdrReady();
        if (configured && !tuned) tuned = espSdrSetFrequencyMHz(kCaptureCenterMHz);
        configured = configured && tuned;
        const uint8_t gain = appliedReceiverGain;
        if (configured && gain != appliedGain) {
            configured = espSdrSetGain(gain) && espSdrSetAgc(false);
            if (configured) {
                appliedGain = gain;
                processor.reset();
            }
        }
        frame.gain = appliedGain < 0 ? gain : (uint8_t)appliedGain;
        uint32_t captureUs = 0;
        if (configured && espSdrAcquireBurst(samples, kSpectrumBins, kCaptureClock, &captureUs)) {
            processor.process(samples, frame);
        } else {
            processor.reset();
            snprintf(frame.error, sizeof(frame.error), "%s (%ld)",
                     espSdrLastError(), (long)espSdrErrorDetail());
        }
        // Log only on a state change, plus a 30 s heartbeat.
        static bool lastValid = false, lastClipped = false;
        static uint8_t lastGain = 255;
        static uint32_t lastLog = 0;
        const bool clipped = frame.clipped != 0;
        if (frame.valid != lastValid || frame.gain != lastGain || clipped != lastClipped
            || millis() - lastLog >= 30000) {
            Serial.printf("[rx] valid=%u gain=%u clipped=%u capture=%luus free=%u %s\n",
                          frame.valid, frame.gain, frame.clipped,
                          (unsigned long)captureUs, ESP.getFreeHeap(), frame.error);
            lastValid = frame.valid;
            lastGain = frame.gain;
            lastClipped = clipped;
            lastLog = millis();
        }
        xQueueOverwrite(frameQueue, &frame);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void cycleTraceMode() {
    traceMode = (traceMode + 1) % kTraceModeCount;
    Serial.printf("[ui] trace mode: %s\n", kTraceModeNames[traceMode]);
}

static void cycleWaterfallStyle() {
    waterfallStyle = (waterfallStyle + 1) % kWfStyleCount;
    Serial.printf("[ui] waterfall style: %s\n", kWaterfallStyleNames[waterfallStyle]);
}

static void setLayout(int layout) {
    if (layout == screenLayout) return;
    screenLayout = layout;
    applyLayout();
    Serial.printf("[ui] layout: %s\n", kLayoutNames[screenLayout]);
}

// A single tap acts on the part of the screen it hits.
static void singleTapAction(int y) {
    if (screenLayout == kLayoutWaterfall || (screenLayout == kLayoutBoth && y >= waterfallTop))
        cycleWaterfallStyle();
    else cycleTraceMode();
}

// Left tick box = full spectrum, right tick box = full waterfall (tick again to
// go back). A double tap anywhere also toggles between both parts and full
// spectrum. A single tap waits briefly (to rule out a double tap), then changes
// the style of the part it hit. BOOT: short press = tap on the spectrum, hold =
// waterfall palette.
constexpr uint32_t kDoubleTapMs = 350;
// Generous: the resistive panel is least accurate near the screen edges.
constexpr int kCheckboxHitWidth = 100;
// Contact counts as continuous across short pressure dropouts of the panel.
constexpr uint32_t kReleaseHoldMs = 100;

static void handleModeInput() {
    static bool wasTouched = false, wasBoot = false, pending = false;
    static uint32_t lastPoll = 0, lastDown = 0, lastContact = 0, bootDownMs = 0, pendingMs = 0;
    static int pendingY = 0;
    const uint32_t now = millis();
    if (pending && (uint32_t)(now - pendingMs) >= kDoubleTapMs) {
        pending = false;
        singleTapAction(pendingY);
    }
    if ((uint32_t)(now - lastPoll) < 15) return;
    lastPoll = now;

    int x = 0, y = 0;
    if (touchRead(x, y)) lastContact = now;
    const bool touched = (uint32_t)(now - lastContact) < kReleaseHoldMs;
    const bool boot = digitalRead(kBootPin) == LOW;
    bool tap = false;
    int tapY = 0;
    if (touched && !wasTouched && (uint32_t)(now - lastDown) > 80) {
        lastDown = now;
        if (y < kPlotTop)
            Serial.printf("[ui] top-strip touch x=%d y=%d (left<%d, right>=%d)\n", x, y,
                          kCheckboxHitWidth, tft.width() - kCheckboxHitWidth);
        if (y < kPlotTop && x < kCheckboxHitWidth) {
            pending = false;
            setLayout(screenLayout == kLayoutSpectrum ? kLayoutBoth : kLayoutSpectrum);
        } else if (y < kPlotTop && x >= tft.width() - kCheckboxHitWidth) {
            pending = false;
            setLayout(screenLayout == kLayoutWaterfall ? kLayoutBoth : kLayoutWaterfall);
        } else if (y >= kPlotTop) {
            tap = true;
            tapY = y;
        }
    }
    if (boot && !wasBoot) bootDownMs = now;
    if (!boot && wasBoot) {
        if ((uint32_t)(now - bootDownMs) > 600 && screenLayout != kLayoutSpectrum) {
            pending = false;
            cycleWaterfallStyle();
        } else {
            tap = true;
            tapY = kPlotTop;
        }
    }
    if (tap) {
        if (pending && (uint32_t)(now - pendingMs) < kDoubleTapMs) {
            pending = false;
            setLayout(screenLayout == kLayoutBoth ? kLayoutSpectrum : kLayoutBoth);
        } else {
            pending = true;
            pendingMs = now;
            pendingY = tapY;
        }
    }
    wasTouched = touched;
    wasBoot = boot;
}

static void handleSerial() {
    static char line[48];
    static size_t used = 0;
    static bool overflow = false;
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') {
            line[used] = '\0';
            if (used && !overflow) {
                if (!strcmp(line, "burst 0") || !strcmp(line, "burst 1")) {
                    burstFilterEnabled = line[6] == '1';
                    Serial.printf("[rx] burst filter %s\n", burstFilterEnabled ? "ON" : "OFF");
                    used = 0;
                    continue;
                }
                if (!strncmp(line, "mode ", 5) && line[5] >= '0' && line[5] < '0' + kTraceModeCount
                    && line[6] == '\0') {
                    traceMode = line[5] - '0';
                    Serial.printf("[ui] trace mode: %s\n", kTraceModeNames[traceMode]);
                    used = 0;
                    continue;
                }
                if (!strncmp(line, "wf ", 3) && line[3] >= '0' && line[3] < '0' + kWfStyleCount
                    && line[4] == '\0') {
                    waterfallStyle = line[3] - '0';
                    Serial.printf("[ui] waterfall style: %s\n", kWaterfallStyleNames[waterfallStyle]);
                    used = 0;
                    continue;
                }
                if (!strncmp(line, "layout ", 7) && line[7] >= '0' && line[7] < '0' + kLayoutCount
                    && line[8] == '\0') {
                    setLayout(line[7] - '0');
                    used = 0;
                    continue;
                }
                Serial.println("Commands: burst 0|1 | layout 0-2 (both|spectrum|waterfall) | mode 0-3 (dots|line|filled|gradient) | "
                               "wf 0-4 (classic|thermal|phosphor|gray|rainbow) | help");
            }
            used = 0; overflow = false;
        } else if (used + 1 < sizeof(line)) line[used++] = ch;
        else overflow = true;
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[rx] QO-100 DATV spectrum/waterfall");
    uint8_t rotation = 1, brightness = 255;
    Preferences settings;
    if (settings.begin("shcfg", true)) {
        rotation = settings.getUChar("rot", 1);
        brightness = settings.getUChar("bright", 255);
        settings.end();
    }
    if (rotation > 3) rotation = 1;
    tft.init();
    tft.setRotation(1);
    // Inverted colours? Change TFT_INVERSION_ON/OFF in platformio.ini.
#ifdef TFT_INVERSION_ON
    tft.invertDisplay(true);
#else
    tft.invertDisplay(false);
#endif
    ledcSetup(kBacklightChannel, 25000, 8);
    ledcAttachPin(TFT_BL, kBacklightChannel);
    ledcWrite(kBacklightChannel, brightness);
    tft.setSwapBytes(true);
    tft.pushImage(0, 0, kSplashWidth, kSplashHeight, kSplashImage);
    tft.setSwapBytes(false);
    delay(5000);
    tft.setRotation(rotation);
    pinMode(kBootPin, INPUT_PULLUP);
    touchBegin(rotation, tft.width(), tft.height());
    tft.fillScreen(color::BG);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color::BRIGHT, color::BG);
    tft.drawString("QO-100 DATV uplink", tft.width() / 2, kTitleY, 2);
    tft.setTextColor(color::MID, color::BG);
    tft.drawString("RX starting", tft.width() / 2, kStatusY, 1);
    applyLayout();
    frameQueue = xQueueCreate(1, sizeof(SpectrumFrame));
    if (!frameQueue) {
        Serial.println("[rx] FATAL: frame queue allocation failed");
        tft.drawString("Queue allocation failed", tft.width() / 2, kStatusY, 1);
        for (;;) delay(1000);
    }
    if (espSdrBegin()) {
        const uint8_t maximumGain = espSdrGainMax();
        if (appliedReceiverGain > maximumGain) appliedReceiverGain = maximumGain;
        Serial.printf("[rx] fixed gain %u (hardware max %u)\n", appliedReceiverGain, maximumGain);
    }
    if (xTaskCreatePinnedToCore(receiverTask, "receiver", 8192, nullptr,
                                2, nullptr, 0) != pdPASS) {
        Serial.println("[rx] FATAL: receiver task allocation failed");
        tft.drawString("Receiver task allocation failed", tft.width() / 2, kStatusY, 1);
        for (;;) delay(1000);
    }
}

void loop() {
    handleModeInput();
    handleSerial();
    SpectrumFrame frame;
    if (xQueueReceive(frameQueue, &frame, 0) == pdTRUE) drawFrame(frame);
    delay(10);
}
