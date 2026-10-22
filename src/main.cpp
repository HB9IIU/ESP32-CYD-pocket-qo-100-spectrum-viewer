
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>
#include "esp_sdr.h"
#include "spectrum.h"
#include "touch_input.h"
#ifdef CYD_4INCH
#include "splash_image_4in.h"
#else
#include "splash_image.h"
#endif

// Fixed receiver gain index (clamped to the hardware maximum at start-up).
constexpr uint8_t kReceiverGain = 30;

namespace color {
constexpr uint16_t BG = 0x0000, DIM = 0x0333, MID = 0x03BF;
constexpr uint16_t BRIGHT = 0x07FF, ERROR = 0xF800;
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t TRACE_GREEN = rgb(170, 230, 120);
const uint16_t FILL_GREEN = rgb(45, 85, 35);
const uint16_t TRACE_AMBER = rgb(255, 190, 75);
const uint16_t FILL_AMBER = rgb(105, 65, 18);
const uint16_t TRACE_CYAN = rgb(110, 220, 255);
const uint16_t FILL_CYAN = rgb(20, 65, 100);
}

// Branding, title, and status occupy the rows above the plot.
constexpr int kTitleY = 11, kStatusY = 29, kPlotTop = 34, kPlotLeft = 6;
// Vertical scale of the spectrum trace (dBFS). Narrower span = more vertical
// resolution. These are the start-up values; the scale then follows the signal.
constexpr int kPlotMaxDbfs = -10, kPlotMinDbfs = -90;
constexpr int kFrequencyStripHeight = 20;
// Ring capacity covers the full-screen waterfall; combined layout uses 32 rows.
constexpr int kWaterfallRowHeight = 2, kMaxWaterfallRows = 100, kNormalWaterfallRows = 32;
constexpr int kBacklightChannel = 0;
// Spectrum trace style, cycled by tapping the plot or serial "mode N".
enum TraceMode {
    kTraceLine, kTraceFilledGreen, kTraceFilledAmber, kTraceFilledCyan,
    kTraceGradient, kTraceModeCount
};
static const char* const kTraceModeNames[kTraceModeCount] =
    {"line", "filled-green", "filled-amber", "filled-cyan", "gradient"};
// Waterfall colour palette, cycled by tapping the lower half of the screen.
enum WaterfallStyle { kWfClassic, kWfThermal, kWfPhosphor, kWfGray, kWfRainbow, kWfStyleCount };
static const char* const kWaterfallStyleNames[kWfStyleCount] =
    {"classic", "thermal", "phosphor", "gray", "rainbow"};
static int waterfallStyle = kWfClassic;
constexpr int kMaxPlotHeight = 480;
constexpr int kMaxScreenWidth = 480;
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
static bool transmissionCenterValid = false;
static int transmissionCenterKHz = (kBandLowKHz + kBandHighKHz) / 2;
static int transmissionSymbolRateKsps = 0;
static char drawnCenterLabel[48] = {};
static uint32_t lastCenterDetectionMs = 0;

constexpr int kCenterDetectMarginDb = 15;
constexpr int kCenterMinBins = 3;
constexpr int kCenterConfirmFrames = 3;
constexpr uint32_t kCenterHoldMs = 3000;
constexpr int kMeasuredBandwidthPerSymbolRatePercent = 105;

static int normalizeSymbolRateKsps(int measuredKsps) {
    static const int nominalRatesKsps[] = {125, 250, 333, 500, 666, 1000};
    constexpr int nominalRateCount =
        sizeof(nominalRatesKsps) / sizeof(nominalRatesKsps[0]);
    int nearestRate = nominalRatesKsps[0];
    int nearestDifference = abs(measuredKsps - nominalRatesKsps[0]);
    for (int nominalRate : nominalRatesKsps) {
        const int difference = abs(measuredKsps - nominalRate);
        if (difference < nearestDifference) {
            nearestRate = nominalRate;
            nearestDifference = difference;
        }
    }
    return measuredKsps >= nominalRatesKsps[0]
            && measuredKsps <= nominalRatesKsps[nominalRateCount - 1]
        ? nearestRate
        : measuredKsps;
}

static void saveDisplayStyles() {
    Preferences settings;
    if (!settings.begin("shcfg", false)) {
        Serial.println("[ui] ERROR: could not open display settings");
        return;
    }
    const bool traceSaved = settings.putUChar("trace", (uint8_t)traceMode) == sizeof(uint8_t);
    const bool waterfallSaved = settings.putUChar("wf", (uint8_t)waterfallStyle) == sizeof(uint8_t);
    settings.end();
    if (!traceSaved || !waterfallSaved)
        Serial.println("[ui] ERROR: failed to save display styles");
}

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

static void updateTransmissionCenter(const uint8_t* values, int lo, int hi) {
    static int candidateKHz = 0, candidateHits = 0;
    static int candidateSymbolRateKsps = 0;
    const uint32_t now = millis();

    const int floorCode = medianCode(values + lo, hi - lo + 1);
    int threshold = floorCode + kCenterDetectMarginDb * 255 / 100;
    if (threshold > 255) threshold = 255;

    int bestStart = 0, bestLength = 0, runStart = -1;
    for (int bin = lo; bin <= hi + 1; ++bin) {
        const bool above = bin <= hi && values[bin] >= threshold;
        if (above && runStart < 0) runStart = bin;
        if (!above && runStart >= 0) {
            const int length = bin - runStart;
            if (length > bestLength) {
                bestStart = runStart;
                bestLength = length;
            }
            runStart = -1;
        }
    }

    if (bestLength >= kCenterMinBins) {
        const int midpointBinTimes2 = 2 * bestStart + bestLength - 1;
        const int measuredKHz = kCaptureLowKHz
            + (midpointBinTimes2 * kCaptureSpanKHz + kSpectrumBins)
                / (2 * kSpectrumBins);
        const int plateauEdge = bestLength / 5;
        const int plateauStart = bestStart + plateauEdge;
        const int plateauLength = bestLength - 2 * plateauEdge;
        const int plateauCode = medianCode(values + plateauStart, plateauLength);
        const int srThreshold = floorCode
            + ((plateauCode - floorCode) * 90 + 50) / 100;
        const int plateauBin = plateauStart + plateauLength / 2;
        int widthStart = plateauBin, widthEnd = plateauBin;
        while (widthStart > lo && values[widthStart - 1] >= srThreshold) --widthStart;
        while (widthEnd < hi && values[widthEnd + 1] >= srThreshold) ++widthEnd;
        const int bandwidthBins = widthEnd - widthStart + 1;
        const int bandwidthKHz =
            (bandwidthBins * kCaptureSpanKHz + kSpectrumBins / 2) / kSpectrumBins;
        const int measuredSymbolRateKsps =
            (bandwidthKHz * 100 + kMeasuredBandwidthPerSymbolRatePercent / 2)
                / kMeasuredBandwidthPerSymbolRatePercent;
        const int maxStableStepKHz = 4 * kCaptureSpanKHz / kSpectrumBins;
        if (!candidateHits || abs(measuredKHz - candidateKHz) > maxStableStepKHz) {
            candidateKHz = measuredKHz;
            candidateSymbolRateKsps = measuredSymbolRateKsps;
            candidateHits = 1;
        } else {
            candidateKHz += (measuredKHz - candidateKHz) / 4;
            candidateSymbolRateKsps += (measuredSymbolRateKsps - candidateSymbolRateKsps) / 4;
            if (candidateHits < kCenterConfirmFrames) ++candidateHits;
        }
        lastCenterDetectionMs = now;
        if (candidateHits >= kCenterConfirmFrames) {
            transmissionCenterKHz = transmissionCenterValid
                ? transmissionCenterKHz + (candidateKHz - transmissionCenterKHz) / 4
                : candidateKHz;
            transmissionSymbolRateKsps = normalizeSymbolRateKsps(candidateSymbolRateKsps);
            transmissionCenterValid = true;
        }
    } else {
        candidateHits = 0;
        if (transmissionCenterValid && now - lastCenterDetectionMs >= kCenterHoldMs)
            transmissionCenterValid = false;
    }
}

// Draw once: this strip is outside the trace and waterfall repaint regions.
static void drawTransmissionCenterLabel() {
    char centerLabel[24], rateLabel[24], label[48];
    if (transmissionCenterValid)
        snprintf(centerLabel, sizeof(centerLabel), "CF = %d.%03d MHz",
                 transmissionCenterKHz / 1000, transmissionCenterKHz % 1000);
    else
        snprintf(centerLabel, sizeof(centerLabel), "CF = --- MHz");
    if (transmissionCenterValid)
        snprintf(rateLabel, sizeof(rateLabel), "SR est. %d ksps", transmissionSymbolRateKsps);
    else
        snprintf(rateLabel, sizeof(rateLabel), "SR = --- ksps");
    snprintf(label, sizeof(label), "%s|%s", centerLabel, rateLabel);
    if (!strcmp(label, drawnCenterLabel)) return;

    constexpr int margin = 8;
    constexpr int gap = 8;
    const int left = margin;
    const int right = tft.width() - margin;
    const int textFont = tft.textWidth(centerLabel, 2)
            + tft.textWidth(rateLabel, 2) + gap <= right - left
        ? 2
        : 1;
    const int stripTop = waterfallTop - kFrequencyStripHeight - 4;
    tft.fillRect(0, stripTop, tft.width(), kFrequencyStripHeight, color::BG);
    tft.setTextColor(color::BRIGHT, color::BG);
    const int textY = stripTop + (kFrequencyStripHeight - (textFont == 2 ? 16 : 8)) / 2;
    tft.setTextDatum(TL_DATUM);
    tft.drawString(centerLabel, left, textY, textFont);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(rateLabel, right, textY, textFont);
    snprintf(drawnCenterLabel, sizeof(drawnCenterLabel), "%s", label);
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
    drawnCenterLabel[0] = '\0';
    tft.fillRect(0, kPlotTop, tft.width(), tft.height() - kPlotTop, color::BG);
    tft.drawFastHLine(kPlotLeft, kPlotTop + plotHeight,
                      tft.width() - kPlotLeft - 6, color::DIM);
    if (waterfallRows > 0)
        tft.drawRect(5, waterfallTop - 1, tft.width() - 10,
                     waterfallRows * kWaterfallRowHeight + 2, color::DIM);
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
            uint16_t traceColor = color::TRACE_GREEN;
            uint16_t fillColor = color::FILL_GREEN;
            if (traceMode == kTraceFilledAmber) {
                traceColor = color::TRACE_AMBER;
                fillColor = color::FILL_AMBER;
            } else if (traceMode == kTraceFilledCyan) {
                traceColor = color::TRACE_CYAN;
                fillColor = color::FILL_CYAN;
            }
            if (traceMode == kTraceFilledGreen || traceMode == kTraceFilledAmber
                || traceMode == kTraceFilledCyan)
                tft.drawFastVLine(x, y, kPlotTop + plotHeight - y, fillColor);
            // Each segment only touches columns x-1 and x, so the per-column
            // clear above never erases it.
            if (pixel > 0) tft.drawLine(x - 1, prevY, x, y, traceColor);
            else tft.drawPixel(x, y, traceColor);
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
    else snprintf(status, sizeof(status), "%d bins | 31.25 kHz spacing", kSpectrumBins);
    static char lastStatus[64] = {};
    static uint16_t lastStatusColor = 0;
    const uint16_t statusColor = frame.valid && !frame.clipped ? color::MID : color::ERROR;
    if (strcmp(status, lastStatus) || statusColor != lastStatusColor) {
        tft.fillRect(0, kStatusY - 5, tft.width(), kPlotTop - kStatusY + 5, color::BG);
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
    if (frame.valid)
        updateTransmissionCenter(smooth, spectrumBinForPixel(0, width),
                                 spectrumBinForPixel(width - 1, width));
    else if (transmissionCenterValid
             && millis() - lastCenterDetectionMs >= kCenterHoldMs) {
        transmissionCenterValid = false;
    }
    drawTransmissionCenterLabel();
    if (plotHeight > 0) drawSpectrumTrace(width, right);

    memcpy(waterfall[waterfallHead], frame.bins, sizeof(frame.bins));
    waterfallHead = (waterfallHead + 1) % kMaxWaterfallRows;
    static uint16_t line[kMaxScreenWidth];
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
    saveDisplayStyles();
    Serial.printf("[ui] trace mode: %s\n", kTraceModeNames[traceMode]);
}

static void cycleWaterfallStyle() {
    waterfallStyle = (waterfallStyle + 1) % kWfStyleCount;
    saveDisplayStyles();
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

// A double tap toggles combined/full-spectrum layout; a single tap changes the
// style of the part it hits.
constexpr uint32_t kDoubleTapMs = 350;
// Contact counts as continuous across short pressure dropouts of the panel.
constexpr uint32_t kReleaseHoldMs = 100;

static void handleModeInput() {
    static bool wasTouched = false, pending = false;
    static uint32_t lastPoll = 0, lastDown = 0, lastContact = 0, pendingMs = 0;
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
    bool tap = false;
    int tapY = 0;
    if (touched && !wasTouched && (uint32_t)(now - lastDown) > 80) {
        lastDown = now;
        tap = y >= kPlotTop;
        tapY = y;
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
                    saveDisplayStyles();
                    Serial.printf("[ui] trace mode: %s\n", kTraceModeNames[traceMode]);
                    used = 0;
                    continue;
                }
                if (!strncmp(line, "wf ", 3) && line[3] >= '0' && line[3] < '0' + kWfStyleCount
                    && line[4] == '\0') {
                    waterfallStyle = line[3] - '0';
                    saveDisplayStyles();
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
                Serial.println("Commands: burst 0|1 | layout 0-2 (both|spectrum|waterfall) | mode 0-4 (line|filled-green|filled-amber|filled-cyan|gradient) | "
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
#ifdef CYD_4INCH
    rotation = 3;
#endif
    Preferences settings;
    if (settings.begin("shcfg", true)) {
        rotation = settings.getUChar("rot", rotation);
        brightness = settings.getUChar("bright", 255);
        const uint8_t savedTraceMode = settings.getUChar("trace", kTraceLine);
        const uint8_t savedWaterfallStyle = settings.getUChar("wf", kWfClassic);
        if (savedTraceMode < kTraceModeCount) traceMode = savedTraceMode;
        if (savedWaterfallStyle < kWfStyleCount) waterfallStyle = savedWaterfallStyle;
        settings.end();
    }
    if (rotation > 3) rotation = 1;
    tft.init();
#ifdef CYD_4INCH
    tft.setRotation(rotation);
#else
    tft.setRotation(1);
#endif
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
    tft.pushImage((tft.width() - kSplashWidth) / 2,
                  (tft.height() - kSplashHeight) / 2,
                  kSplashWidth, kSplashHeight, kSplashImage);
    tft.setSwapBytes(false);
    delay(5000);
    tft.setRotation(rotation);
    rotation = touchBegin(tft, rotation, tft.width(), tft.height());
    tft.setRotation(rotation);
    tft.fillScreen(color::BG);
    const char* const title = "QO-100 Uplink Spectrum Viewer";
    const int titleFont = tft.textWidth(title, 2) <= tft.width() - 8 ? 2 : 1;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color::MID, color::BG);
    char versionLabel[24];
    snprintf(versionLabel, sizeof(versionLabel), "Ver. %s", APP_VERSION);
    tft.drawString(versionLabel, 4, 0, 1);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("by HB9IIU", tft.width() - 4, 0, 1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color::BRIGHT, color::BG);
    tft.drawString(title, tft.width() / 2, kTitleY, titleFont);
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
