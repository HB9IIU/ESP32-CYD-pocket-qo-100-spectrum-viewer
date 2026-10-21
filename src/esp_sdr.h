#pragma once
#include <stddef.h>
#include <stdint.h>

// Original ESP32 only. The capture aperture is reserved before heap startup.
bool espSdrBegin();
bool espSdrReady();
const char* espSdrLastError();
int32_t espSdrErrorDetail();
uint8_t espSdrGainMax();
// Retune between Wi-Fi channel centres. Successful return means configured,
// not verified PLL lock; an external RF source is needed to verify tuning.
bool espSdrSetFrequencyMHz(uint32_t mhz);
bool espSdrSetAgc(bool on);
bool espSdrSetGain(uint8_t gain);

// Call from the receiver task. Clock: 0=16, 1=80, 2=40 MS/s.
// Each word contains signed 10-bit I (bits 0..9) and Q (bits 10..19).
bool espSdrAcquireBurst(uint32_t* output, size_t count, uint8_t clock,
                        uint32_t* elapsedUs);
