#include "touch_input.h"
#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

namespace {
// The 2.8-inch CYD touch controller uses VSPI, separate from the LCD's HSPI.
constexpr int kCs = 33, kIrq = 36;
// Idle readings are below 30; real touches dip to ~450 at the screen corners.
constexpr int kPressureThreshold = 350;
#ifndef CYD_4INCH
constexpr int kClock = 25, kMosi = 32, kMiso = 39;
SPIClass touchSpi(VSPI);
#else
TFT_eSPI* touchDisplay;
#endif
uint8_t screenRotation = 1;
int screenWidth = 320, screenHeight = 240;
struct Calibration {
    uint8_t swapXY;
    uint16_t rawAtX0, rawAtX1, rawAtY0, rawAtY1;
} calibration = {};
bool calibrated = false;

#ifndef CYD_4INCH
uint16_t readChannel(uint8_t command) {
    touchSpi.transfer(command);
    return (touchSpi.transfer16(0) >> 3) & 4095;
}
#endif

int median3(int a, int b, int c) {
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    return a > b ? a : b;
}

#ifdef CYD_4INCH
bool loadFourInchCalibration(uint16_t (&data)[5]) {
    Preferences settings;
    if (!settings.begin("TFT", true)) {
        Serial.println("[touch] ERROR: could not read calibration settings");
        return false;
    }
    bool valid = true;
    for (int i = 0; i < 5; ++i) {
        const uint32_t value = settings.getUInt(("calib" + String(i)).c_str(), UINT32_MAX);
        if (value > UINT16_MAX) valid = false;
        else data[i] = (uint16_t)value;
    }
    settings.end();
    if (valid) {
        valid = data[0] > 0 && data[0] <= 4095
            && data[1] > 500 && data[1] <= 4095
            && data[2] > 0 && data[2] <= 4095
            && data[3] > 500 && data[3] <= 4095
            && data[4] <= 7;
    }
    return valid;
}

void saveFourInchCalibration(const uint16_t (&data)[5]) {
    Preferences settings;
    if (!settings.begin("TFT", false)) {
        Serial.println("[touch] ERROR: could not open calibration settings");
        return;
    }
    bool saved = true;
    for (int i = 0; i < 5; ++i)
        saved = settings.putUInt(("calib" + String(i)).c_str(), data[i]) == sizeof(uint32_t)
            && saved;
    settings.end();
    if (!saved) Serial.println("[touch] ERROR: failed to save complete calibration");
}

void saveFourInchRotation(uint8_t rotation) {
    Preferences settings;
    if (!settings.begin("shcfg", false)) {
        Serial.println("[touch] ERROR: could not open rotation settings");
        return;
    }
    if (settings.putUChar("rot", rotation) != sizeof(rotation))
        Serial.println("[touch] ERROR: could not save rotation setting");
    settings.end();
}

void showFourInchOrientationCheck(TFT_eSPI& display, uint8_t& rotation) {
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(TC_DATUM);
    display.setTextFont(2);
    display.setTextColor(TFT_GOLD, TFT_BLACK);
    display.drawString("TOUCHSCREEN ORIENTATION", display.width() / 2, 24);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("Is this text upside-down?", display.width() / 2, 82);
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.drawString("Tap anywhere to flip it", display.width() / 2, 116);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString("Or wait for the countdown", display.width() / 2, 146);

    bool flip = false;
    for (int seconds = 8; seconds > 0 && !flip; --seconds) {
        display.fillRect(display.width() / 2 - 20, 184, 40, 52, TFT_BLACK);
        display.setTextFont(4);
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawNumber(seconds, display.width() / 2, 190);
        const uint32_t start = millis();
        while ((uint32_t)(millis() - start) < 1000) {
            if (display.getTouchRawZ() > 300) {
                flip = true;
                break;
            }
            delay(20);
        }
        display.setTextFont(2);
    }

    if (!flip) return;
    rotation = rotation == 3 ? 1 : 3;
    display.setRotation(rotation);
    saveFourInchRotation(rotation);
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString("Orientation flipped", display.width() / 2, display.height() / 2 - 8);
    delay(1000);
    while (display.getTouchRawZ() > 300) delay(20);
}

void runFourInchCalibration(TFT_eSPI& display, uint8_t& rotation) {
    showFourInchOrientationCheck(display, rotation);

    display.fillScreen(TFT_BLACK);
    display.setTextDatum(TC_DATUM);
    display.setTextFont(2);
    display.setTextColor(TFT_GOLD, TFT_BLACK);
    display.drawString("TOUCHSCREEN CALIBRATION", display.width() / 2, 24);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("Tap the arrows as they appear", display.width() / 2, 94);
    display.drawString("in each corner to calibrate", display.width() / 2, 122);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString("Tap anywhere to begin", display.width() / 2, 174);
    uint16_t x = 0, y = 0;
    while (!display.getTouch(&x, &y)) delay(10);
    while (display.getTouchRawZ() > 300) delay(20);

    uint16_t data[5] = {};
    display.calibrateTouch(data, TFT_GREEN, TFT_BLACK, 12);
    saveFourInchCalibration(data);
    display.setTouch(data);
    Serial.println("[touch] 4-inch touch calibration saved and applied");

    display.fillScreen(TFT_BLACK);
    display.setTextFont(2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("Calibration complete", display.width() / 2, 16);
    display.drawString("Tap to test touch position", display.width() / 2, 42);
    display.fillRoundRect(155, display.height() - 48, 170, 36, 8, TFT_BLUE);
    display.drawRoundRect(155, display.height() - 48, 170, 36, 8, TFT_WHITE);
    display.setTextColor(TFT_WHITE, TFT_BLUE);
    display.drawString("Continue", 240, display.height() - 40);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    for (;;) {
        if (display.getTouch(&x, &y)) {
            display.fillCircle(x, y, 3, TFT_RED);
            if (x >= 155 && x < 325 && y >= display.height() - 48) break;
            display.fillRect(100, display.height() / 2 - 16,
                             display.width() - 200, 32, TFT_BLACK);
            display.drawString("x=" + String(x) + "  y=" + String(y),
                               display.width() / 2, display.height() / 2 - 8);
            while (display.getTouchRawZ() > 300) delay(20);
        }
        delay(10);
    }
    while (display.getTouchRawZ() > 300) delay(20);
    display.fillScreen(TFT_BLACK);
}

void loadOrCalibrateFourInchTouch(TFT_eSPI& display, uint8_t& rotation) {
    uint16_t data[5] = {};
    if (loadFourInchCalibration(data)) {
        display.setTouch(data);
        Serial.println("[touch] 4-inch touch calibration loaded");
        return;
    }
    Serial.println("[touch] no 4-inch calibration found; starting first-boot setup");
    runFourInchCalibration(display, rotation);
}
#endif

// Idle probe: shows whether the controller answers even while IRQ stays high.
void logIdleProbe() {
#ifdef CYD_4INCH
    const uint16_t pressure = touchDisplay->getTouchRawZ();
    uint16_t rawX = 0, rawY = 0;
    touchDisplay->getTouchRaw(&rawX, &rawY);
    Serial.printf("[touch] idle irq=HIGH pressure=%u rawX=%u rawY=%u (TFT SPI)\n",
                  pressure, rawX, rawY);
#else
    touchSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(kCs, LOW);
    const int z1 = readChannel(0xB0), z2 = readChannel(0xC0);
    const int rawX = readChannel(0x90), rawY = readChannel(0xD0);
    digitalWrite(kCs, HIGH);
    touchSpi.endTransaction();
    Serial.printf("[touch] idle irq=HIGH z1=%d z2=%d pressure=%d rawX=%d rawY=%d\n",
                  z1, z2, 4095 + z1 - z2, rawX, rawY);
#endif
}
}

uint8_t touchBegin(TFT_eSPI& display, uint8_t rotation, int width, int height) {
    screenRotation = rotation & 3;
    screenWidth = width;
    screenHeight = height;
    pinMode(kIrq, INPUT);
#ifdef CYD_4INCH
    touchDisplay = &display;
    Serial.printf("[touch] using TFT SPI touch controller cs=%d irq=%d irqLevel=%d\n",
                  kCs, kIrq, digitalRead(kIrq));
    loadOrCalibrateFourInchTouch(display, rotation);
    screenRotation = rotation;
    screenWidth = display.width();
    screenHeight = display.height();
    return rotation;
#else
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    touchSpi.begin(kClock, kMiso, kMosi, kCs);
    Serial.printf("[touch] begin rot=%u screen=%dx%d pins cs=%d clk=%d mosi=%d miso=%d irq=%d irqLevel=%d\n",
                  screenRotation, width, height, kCs, kClock, kMosi, kMiso, kIrq, digitalRead(kIrq));
#endif

    // Reuse original firmware calibration if available; never interrupt boot
    // with a calibration screen. Relative swipes also work with default axes.
    Preferences settings;
    if (settings.begin("shdr", true)) {
        if (settings.isKey("cal") && settings.getBytesLength("cal") == sizeof(calibration)) {
            settings.getBytes("cal", &calibration, sizeof(calibration));
            calibrated = calibration.swapXY <= 1
                && calibration.rawAtX0 <= 4095 && calibration.rawAtX1 <= 4095
                && calibration.rawAtY0 <= 4095 && calibration.rawAtY1 <= 4095
                && abs((int)calibration.rawAtX1 - calibration.rawAtX0) > 500
                && abs((int)calibration.rawAtY1 - calibration.rawAtY0) > 500;
        }
        settings.end();
    }
    if (calibrated)
        Serial.printf("[touch] saved calibration: swapXY=%u X %u..%u Y %u..%u\n",
                      calibration.swapXY, calibration.rawAtX0, calibration.rawAtX1,
                      calibration.rawAtY0, calibration.rawAtY1);
    else Serial.println("[touch] no valid saved calibration, using default axes");
    return rotation;
}

bool touchRead(int& x, int& y) {
    static uint32_t lastIdleLog = 0, lastLog = 0;
    const uint32_t now = millis();
#ifdef CYD_4INCH
    uint16_t touchX = 0, touchY = 0;
    if (!touchDisplay->getTouch(&touchX, &touchY, kPressureThreshold)) {
        if ((uint32_t)(now - lastIdleLog) >= 5000) {
            lastIdleLog = now;
            logIdleProbe();
        }
        return false;
    }
    x = touchX;
    y = touchY;
    if ((uint32_t)(now - lastLog) >= 100) {
        lastLog = now;
        Serial.printf("[touch] calibrated x=%d y=%d pressure=%u\n",
                      x, y, touchDisplay->getTouchRawZ());
    }
    return true;
#else
    if (digitalRead(kIrq) != LOW) {
        if ((uint32_t)(now - lastIdleLog) >= 5000) {
            lastIdleLog = now;
            logIdleProbe();
        }
        return false;
    }
    touchSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(kCs, LOW);
    const int z1 = readChannel(0xB0), z2 = readChannel(0xC0);
    const int pressure = 4095 + z1 - z2;
    int rawX = 0, rawY = 0;
    if (pressure > kPressureThreshold && pressure <= 4095) {
        // Discard first coordinate conversion, then reject single-sample spikes.
        readChannel(0x90);
        const int x0 = readChannel(0x90), y0 = readChannel(0xD0);
        const int x1 = readChannel(0x90), y1 = readChannel(0xD0);
        const int x2 = readChannel(0x90), y2 = readChannel(0xD0);
        rawX = median3(x0, x1, x2);
        rawY = median3(y0, y1, y2);
    }
    digitalWrite(kCs, HIGH);
    touchSpi.endTransaction();
    if (pressure <= kPressureThreshold || pressure > 4095
        || rawX < 80 || rawX > 4015 || rawY < 80 || rawY > 4015) {
        if ((uint32_t)(now - lastLog) >= 250) {
            lastLog = now;
            Serial.printf("[touch] irq=LOW but rejected: pressure=%d (need %d..4095) rawX=%d rawY=%d\n",
                          pressure, kPressureThreshold + 1, rawX, rawY);
        }
        return false;
    }

    if (calibrated) {
        const int axisX = calibration.swapXY ? rawY : rawX;
        const int axisY = calibration.swapXY ? rawX : rawY;
        x = map(axisX, calibration.rawAtX0, calibration.rawAtX1, 30, screenWidth - 30);
        y = map(axisY, calibration.rawAtY0, calibration.rawAtY1, 30, screenHeight - 30);
    } else {
        int axisX, axisY;
        switch (screenRotation) {
            case 0: axisX = 4095 - rawY; axisY = rawX; break;
            case 1: axisX = rawX; axisY = rawY; break;
            case 2: axisX = rawY; axisY = 4095 - rawX; break;
            default: axisX = 4095 - rawX; axisY = 4095 - rawY; break;
        }
        x = map(axisX, 200, 3900, 0, screenWidth - 1);
        y = map(axisY, 200, 3900, 0, screenHeight - 1);
    }
    x = constrain(x, 0, screenWidth - 1);
    y = constrain(y, 0, screenHeight - 1);
    if ((uint32_t)(now - lastLog) >= 100) {
        lastLog = now;
        Serial.printf("[touch] raw=(%d,%d) pressure=%d -> x=%d y=%d (%s)\n",
                      rawX, rawY, pressure, x, y, calibrated ? "calibrated" : "default");
    }
    return true;
#endif
}
