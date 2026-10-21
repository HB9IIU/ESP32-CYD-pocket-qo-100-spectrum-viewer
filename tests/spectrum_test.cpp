#include "spectrum.h"
#include <assert.h>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <cstring>

static uint32_t pack(int i, int q) {
    return (i & 1023) | ((q & 1023) << 10);
}
static void tone(uint32_t* data, int bin, float amplitude, int dcI = 0, int dcQ = 0) {
    for (int n = 0; n < kSpectrumBins; ++n) {
        const double phase = 2 * std::acos(-1.0) * bin * n / kSpectrumBins;
        data[n] = pack((int)std::lround(amplitude * std::cos(phase)) + dcI,
                       (int)std::lround(-amplitude * std::sin(phase)) + dcQ);
    }
}
static int peak(const SpectrumFrame& frame) {
    int bin = 0;
    for (int n = 1; n < kSpectrumBins; ++n)
        if (frame.bins[n] > frame.bins[bin]) bin = n;
    return bin;
}
static void addBroadBurst(uint32_t* data) {
    uint32_t random = 0x12345678;
    for (int n = 0; n < kSpectrumBins; ++n) {
        int i = data[n] & 1023, q = (data[n] >> 10) & 1023;
        if (i >= 512) i -= 1024;
        if (q >= 512) q -= 1024;
        random = random * 1664525u + 1013904223u;
        i += (int)((random >> 16) % 401) - 200;
        random = random * 1664525u + 1013904223u;
        q += (int)((random >> 16) % 401) - 200;
        data[n] = pack(i, q);
    }
}
int main() {
    static_assert(kCaptureSpanKHz * 1000 / kSpectrumBins == 31250,
                  "16 MHz capture with FFT 512 must give 31.25 kHz bins");
    static_assert(kCaptureClock == 0, "16 MS/s requires hardware clock 0");
    SpectrumProcessor processor;
    uint32_t data[kSpectrumBins];
    SpectrumFrame frame;
    // Both sides of the LO must point in the correct RF direction.
    for (int bin : {-37, 37}) {
        processor.reset();
        tone(data, bin, 256);
        processor.process(data, frame);
        const int expected = (bin + kSpectrumBins / 2 + kSpectrumBins) % kSpectrumBins;
        assert(frame.valid && peak(frame) == expected && frame.clipped == 0);
        const float dbfs = -100 + frame.bins[expected] * 100.0f / 255;
        assert(std::fabs(dbfs + 6.0206f) < 0.5f);
        const int fullCode = frame.bins[expected];
        processor.reset();
        tone(data, bin, 128);
        processor.process(data, frame);
        assert(std::fabs((fullCode - frame.bins[expected]) * 100.0f / 255 - 6.0206f) < 0.5f);
        // Removing a constant I/Q offset must preserve the carrier level.
        const int halfCode = frame.bins[expected];
        processor.reset();
        tone(data, bin, 128, 50, -80);
        processor.process(data, frame);
        assert(std::abs((int)frame.bins[expected] - halfCode) <= 1);
    }
    // Reset drops previous averaging; silence contains no artificial peaks.
    for (auto& word : data) word = pack(0, 0);
    processor.reset();
    processor.process(data, frame);
    for (auto code : frame.bins) assert(code == 0);
    assert(frame.clipped == 0);
    for (auto& word : data) word = pack(-512, 511);
    processor.process(data, frame);
    assert(frame.clipped == kSpectrumBins);
    // DATV crop spans the same bins at both supported screen orientations.
    for (int width : {228, 308}) {
        assert(spectrumBinForPixel(0, width) == 96);
        assert(spectrumBinForPixel(width - 1, width) == 384);
        for (int x = 1; x < width; ++x) {
            assert(spectrumBinForPixel(x, width) >= spectrumBinForPixel(x - 1, width));
            assert(spectrumBinForPixel(x, width) < kSpectrumBins);
        }
    }
    // A 2402 MHz source is 4 MHz below the 2406 MHz LO: FFT bin -128.
    processor.reset();
    tone(data, -128, 128);
    processor.process(data, frame);
    assert(peak(frame) == 128);
    assert(kCaptureLowKHz + peak(frame) * kCaptureSpanKHz / kSpectrumBins == 2402000);
    // A 2409 MHz source is 3 MHz above the LO: FFT bin +96.
    processor.reset();
    tone(data, 96, 128);
    processor.process(data, frame);
    assert(peak(frame) == 352);
    assert(kCaptureLowKHz + peak(frame) * kCaptureSpanKHz / kSpectrumBins == 2409000);
    // Regression: the 2409.75 MHz / 10499.25 MHz channel was outside the old crop.
    processor.reset();
    tone(data, 120, 128);
    processor.process(data, frame);
    assert(peak(frame) == 376);
    assert(kCaptureLowKHz + peak(frame) * kCaptureSpanKHz / kSpectrumBins == 2409750);
    bool rightChannelVisible = false;
    for (int x = 0; x < 308; ++x)
        if (spectrumBinForPixel(x, 308) == peak(frame)) rightChannelVisible = true;
    assert(rightChannelVisible);
    // Preserve a continuous carrier under three consecutive broadband bursts.
    processor.reset();
    tone(data, -37, 64);
    for (int n = 0; n < kBurstFilterFrames; ++n) processor.process(data, frame);
    uint8_t baseline[kSpectrumBins];
    std::memcpy(baseline, frame.bins, sizeof(baseline));
    addBroadBurst(data);
    for (int n = 0; n < 3; ++n) {
        processor.process(data, frame);
        assert(frame.burstFiltered);
        assert(std::memcmp(frame.bins, baseline, sizeof(baseline)) == 0);
    }
    tone(data, -37, 64);
    for (int n = 0; n < kBurstFilterFrames; ++n) {
        processor.process(data, frame);
        assert(std::memcmp(frame.bins, baseline, sizeof(baseline)) == 0);
    }
    // Comparison mode must expose the burst rather than silently filter it.
    processor.setBurstFilterEnabled(false);
    for (int n = 0; n < kBurstFilterFrames; ++n) processor.process(data, frame);
    addBroadBurst(data);
    processor.process(data, frame);
    int raised = 0;
    for (int bin = 0; bin < kSpectrumBins; ++bin)
        if (frame.bins[bin] > baseline[bin] + 20) ++raised;
    assert(!frame.burstFiltered && raised > 100);
    // New steady signals must still appear after the short filter delay.
    processor.setBurstFilterEnabled(true);
    for (auto& word : data) word = pack(0, 0);
    for (int n = 0; n < kBurstFilterFrames; ++n) processor.process(data, frame);
    tone(data, 37, 128);
    for (int n = 0; n < 30; ++n) processor.process(data, frame);
    assert(peak(frame) == kSpectrumBins / 2 + 37);
    const float steadyDb = -100 + frame.bins[peak(frame)] * 100.0f / 255;
    assert(std::fabs(steadyDb + 12.0412f) < 0.5f);
    std::puts("PASS: DSP, DATV mapping, burst rejection, continuous signal preservation, filter bypass");
}
