# QO-100 DATV spectrum and waterfall

A standalone, receive-only viewer for the Classic 2.8-inch ESP32 Cheap Yellow
Display (ESP32-2432S028R). It boots straight into the local QO-100 DATV uplink
band, **2401.0-2410.0 MHz**, using the ESP32's own Wi-Fi radio as the receiver.
No external SDR, no transmitter, no simulated data.

## Screen

- Spectrum trace on top, waterfall below, frequency labels in between.
- **Tick box top-left:** spectrum over the whole page. **Tick box top-right:**
  waterfall over the whole page. Tick again to go back. Double tap anywhere
  toggles between both parts and full spectrum.
- **Tap the spectrum** to cycle its style: dots, line, filled, gradient.
  **Tap the waterfall** to cycle its palette: classic, thermal, phosphor, gray,
  rainbow. A single tap acts after 350 ms, to rule out a double tap.
- **BOOT:** short press = spectrum style, hold = waterfall palette,
  two short presses = layout toggle.
- **Auto-scale:** a transmission (a run of at least 8 bins, 10 dB above the
  floor) is detected and its plateau is placed at 95% of the plot height.
  The bottom of the scale follows the noise floor. The scale moves slowly and
  returns to the default 10 s after the transmission ends.
- The header shows the bin width; CLIPPING appears when the ADC saturates.

## Gain

Receiver gain is fixed: `kReceiverGain = 30` at the top of `src/main.cpp`
(clamped to the hardware maximum). Gain indices are not dB.

## USB serial (115200 baud)

| Command | Effect |
|---|---|
| `layout 0\|1\|2` | both / full spectrum / full waterfall |
| `mode 0-3` | dots / line / filled / gradient |
| `wf 0-4` | classic / thermal / phosphor / gray / rainbow |
| `burst 0\|1` | burst suppression off / on (resets at boot) |

## Build and flash

```sh
pio run -j 2
pio run -t upload --upload-port COM34 -j 2
pio device monitor --port COM34 --baud 115200
```

Or flash the prebuilt image `dist/cyd-spectrum_0x0.bin` at address 0x0:

```sh
esptool.py --chip esp32 --port COM34 write_flash 0x0 dist/cyd-spectrum_0x0.bin
```

This build is for the original ESP32 Classic CYD, not ESP32-S3. Change COM34
if Windows assigns a different port.

**Inverted colours:** if the plot background is white and the waterfall pink,
change `-D TFT_INVERSION_ON=1` to `-D TFT_INVERSION_OFF=1` in `platformio.ini`
(keep only one), then rebuild and flash.

## How it works

Core 0 captures raw 10-bit I/Q at **16 MS/s** with the receive PLL requested at
**2406 MHz**; core 1 draws the latest frame. A 512-point Hann-window FFT gives
**31.25 kHz** bins. Each bin is the median of the last 7 captures (burst
suppression, which can hide very short transmissions), then averaged
(`kPowerAverageAlpha` in `src/spectrum.h`) and shown in dBFS. The LO bin at
2406 MHz is interpolated.

- `src/main.cpp`: display, layouts, auto-scale, touch/BOOT/serial input, RF task.
- `src/spectrum.cpp` / `.h`: FFT, burst suppression, averaging.
- `src/esp_sdr.cpp` / `.h`: raw I/Q capture and gain control.
- `src/touch_input.cpp` / `.h`: XPT2046 touch on VSPI (pins 25/32/39/33, IRQ 36).
- `scripts/spectrum_memory.ld`: stops the firmware overlapping the 64 KiB
  capture SRAM. Do not remove it.
- `tests/`: portable DSP and gesture tests
  (`g++ -std=c++17 -Isrc src/spectrum.cpp tests/spectrum_test.cpp`).

## Measurement limits

Levels are **dBFS**, not calibrated input dBm. Capture uses snapshots with gaps,
so brief bursts can be missed. The viewer does not decode DATV and does not
receive the 10 GHz downlink. The displayed range matches the
[AMSAT-DL WB bandplan V3](https://amsat-dl.org/wp-content/uploads/2021/02/QO-100-WB-Bandplan-V3.pdf):
uplink 2401.0-2410.0 MHz maps to downlink 10490.5-10499.5 MHz.

The ROM tuning call gives no reliable PLL-lock result, so a successful capture
alone does not prove the LO position.

## First RF check

Use a weak known source inside the displayed band. Compare source off and on,
change its frequency and check that the feature moves the same way, then repeat
at lower gain (edit `kReceiverGain`). Never connect a transmitter output
directly to the receiver antenna input.

## License and attribution

Apache-2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Receiver research: [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr).
