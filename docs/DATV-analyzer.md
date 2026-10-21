# Basic QO-100 uplink analyzer

Receive-only ESP32/CYD receiver for viewing local RF at 2401.0–2410.0 MHz.
It boots directly into a trace and waterfall. It does not receive the satellite
downlink or decode DATV video.

- Corrected signed 10-bit I/Q extraction and spectrum frequency direction.
- Fixed Hann-normalized dBFS scale (-100 to 0), not calibrated antenna dBm.
- Reserved hardware capture SRAM and linker overlap guards.
- Manual gain starts at 40. Swipe right for more gain, left for less.
  Gain is shown briefly after adjustment; the normal header shows bin width.
  Taps and vertical swipes do not change gain.
- BOOT remains an alternative: increase by 10, wrapping to 0.
  Do not hold BOOT while resetting. Lower gain when CLIPPING appears.
- USB serial at 115200 baud: `rxgain 20` sets an exact gain index.
  Gain indices are not dB.
- Seven-capture burst suppression is enabled by default. Serial
  `burst 0` disables it; `burst 1` enables it. Both plots are filtered.
  It preserves steady signals after a short delay, but cannot reject
  sustained interference and can hide brief transmissions.
- Acquisition failures show the failing step and code, never artificial signals.
- Receiver startup uses reduced Wi-Fi packet pools to avoid ESP_ERR_NO_MEM.
  Failure messages name the failed step
  and show its error code; capture timeouts attempt to restore the RX path.
- Experimental 16 MS/s capture at a requested 2406 MHz, FFT 512;
  displayed bin spacing is 31.25 kHz.
  Useful for broad signals, not precise narrowband measurements.

## Flash

Flash `dist/cyd-spectrum-16mhz-512fft_0x0.bin` for the Classic CYD at address 0x0.
The previous 40 MHz build is preserved as `dist/cyd-spectrum-40mhz_0x0.bin`.
Direct tuning is experimental until verified with an external known signal;
see the calibration and filter limitations in the README.

## First RF check

Use a weak known source within the displayed band. Compare source off/on,
then change its frequency and verify the feature moves in the same direction.
Repeat at lower receiver gain. Do not connect a transmitter output directly
to the receiver antenna input.

Build and mathematical checks do not establish hardware sensitivity,
frequency accuracy, or absolute power calibration. Hardware testing remains.

Receiver research: https://github.com/ESPARGOS/esp-sdr

Spectrum reference: https://github.com/ESPARGOS/esp-web-sdr
