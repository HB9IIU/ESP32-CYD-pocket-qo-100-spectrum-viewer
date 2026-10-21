#include "touch_input.h"
#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>

namespace {
// Classic CYD XPT2046 uses its own VSPI pins, separate from the LCD's HSPI.
constexpr int kCs = 33, kClock = 25, kMosi = 32, kMiso = 39, kIrq = 36;
// Idle readings are below 30; real touches dip to ~450 at the screen corners.
constexpr int kPressureThreshold = 350;
SPIClass touchSpi(VSPI);
uint8_t screenRotation = 1;
int screenWidth = 320, screenHeight = 240;
struct Calibration {
    uint8_t swapXY;
    uint16_t rawAtX0, rawAtX1, rawAtY0, rawAtY1;
} calibration = {};
bool calibrated = false;

uint16_t readChannel(uint8_t command) {
    touchSpi.transfer(command);
    return (touchSpi.transfer16(0) >> 3) & 4095;
}

int median3(int a, int b, int c) {
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    return a > b ? a : b;
}

// Idle probe: shows whether the controller answers even while IRQ stays high.
void logIdleProbe() {
    touchSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(kCs, LOW);
    const int z1 = readChannel(0xB0), z2 = readChannel(0xC0);
    const int rawX = readChannel(0x90), rawY = readChannel(0xD0);
    digitalWrite(kCs, HIGH);
    touchSpi.endTransaction();
    Serial.printf("[touch] idle irq=HIGH z1=%d z2=%d pressure=%d rawX=%d rawY=%d\n",
                  z1, z2, 4095 + z1 - z2, rawX, rawY);
}
}

void touchBegin(uint8_t rotation, int width, int height) {
    screenRotation = rotation & 3;
    screenWidth = width;
    screenHeight = height;
    pinMode(kIrq, INPUT);
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    touchSpi.begin(kClock, kMiso, kMosi, kCs);
    Serial.printf("[touch] begin rot=%u screen=%dx%d pins cs=%d clk=%d mosi=%d miso=%d irq=%d irqLevel=%d\n",
                  screenRotation, width, height, kCs, kClock, kMosi, kMiso, kIrq, digitalRead(kIrq));

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
}

bool touchRead(int& x, int& y) {
    static uint32_t lastIdleLog = 0, lastLog = 0;
    const uint32_t now = millis();
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
}
