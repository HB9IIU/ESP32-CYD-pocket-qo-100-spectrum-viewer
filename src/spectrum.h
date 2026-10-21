#pragma once
#include <stdint.h>

constexpr int kSpectrumBins = 512;
constexpr int kBurstFilterFrames = 7;
// Exponential power averaging weight per frame (lower = smoother, slower).
constexpr float kPowerAverageAlpha = 0.15f;
constexpr int kCaptureCenterMHz = 2406;
constexpr int kCaptureSpanKHz = 16000;
constexpr uint8_t kCaptureClock = 0;  // ESP32 hardware clock: 16 MS/s.
constexpr int kCaptureLowKHz = kCaptureCenterMHz * 1000 - kCaptureSpanKHz / 2;
// QO-100 WB bandplan V3: 10490.5..10499.5 MHz downlink, minus 8089.5 MHz.
constexpr int kBandLowKHz = 2401000;
constexpr int kBandHighKHz = 2410000;
static_assert(kBandLowKHz >= kCaptureLowKHz &&
              kBandHighKHz < kCaptureLowKHz + kCaptureSpanKHz,
              "Capture must contain the entire DATV band");

struct SpectrumFrame {
    uint8_t bins[kSpectrumBins] = {};
    bool valid = false;
    bool burstFiltered = false;
    uint16_t clipped = 0;
    uint8_t gain = 40;
    char error[48] = {};
};

// Shared mapping keeps the plot and waterfall on exactly the same RF axis.
int spectrumBinForPixel(int pixel, int width);

class SpectrumProcessor {
public:
    SpectrumProcessor();
    void reset();
    void setBurstFilterEnabled(bool enabled);
    void process(const uint32_t* samples, SpectrumFrame& frame);

private:
    void fft();
    float re_[kSpectrumBins] = {};
    float im_[kSpectrumBins] = {};
    float power_[kSpectrumBins] = {};
    // Median instantaneous power BEFORE averaging, so bursts leave no EMA tail.
    float history_[kBurstFilterFrames][kSpectrumBins] = {};
    float window_[kSpectrumBins] = {};
    float twRe_[kSpectrumBins / 2] = {};
    float twIm_[kSpectrumBins / 2] = {};
    bool primed_ = false;
    bool burstFilterEnabled_ = true;
    int historyHead_ = 0;
    int historyCount_ = 0;
};
