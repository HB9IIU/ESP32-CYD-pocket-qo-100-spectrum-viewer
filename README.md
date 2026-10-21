# Pocket QO-100 Spectrum Viewer

A standalone receive-only spectrum viewer for the Classic 2.8-inch ESP32 Cheap Yellow Display (CYD, ESP32-2432S028R). It listens to the local QO-100 DATV uplink band and displays a live spectrum trace plus waterfall on the built-in TFT screen.

This project is designed for experimentation and RF visualization on the bench, not for calibrated power measurements or satellite decoding.

## Features

- Live spectrum trace and waterfall display on the CYD screen
- A 5-second RGB565 boot splash screen
- QO-100 uplink focus: approximately 2401.0–2410.0 MHz
- Built-in auto-scaling to keep active transmissions near the top of the plot
- Touch controls for changing trace mode, waterfall palette, and layout
- Serial commands for quick tuning of display behavior
- Burst suppression and smoothing options for better viewing of steady signals
- PlatformIO build system for ESP32 development

## How it works (beginner-friendly)

The original ESP32 has a 2.4 GHz radio built in for Wi-Fi. This project uses
that radio as a simple receiver instead of using it to join a Wi-Fi network:
it does not connect to your router or scan for Wi-Fi networks.

The firmware asks the radio to listen near the QO-100 DATV uplink frequencies
and captures short bursts of raw signal samples. These samples describe the
radio signal's strength and phase over time. The ESP32 then performs an FFT,
which turns those samples into a picture of signal strength across frequency.
The screen draws that picture as a spectrum trace and adds successive captures
to the waterfall, where stronger signals appear in brighter colors.

This is an experimental reuse of the ESP32's Wi-Fi receiver, not a normal
software-defined radio interface. The radio was designed for Wi-Fi, so tuning,
frequency accuracy, bandwidth, and signal levels have limitations and should
be checked with a known RF source. The displayed levels are relative (dBFS),
not calibrated power in dBm.

### Quick, wireless check for nearby RF

You can use the viewer as a simple **“is it transmitting?”** indicator for a
Pluto SDR or another transceiver: tune the transmitter to a frequency in the
displayed band, key it or start its transmission, and place the CYD nearby.
There is no cable or electrical connection between the transmitter and the
CYD; the ESP32 listens for RF energy radiated nearby.

This is only a rough presence check. It cannot confirm the transmitter's
output power, signal quality, or correct frequency, and shielding, antenna
placement, distance, and receiver settings can all affect what appears on the
screen. **Never connect a transmitter output directly to the CYD.** For a
conducted-output measurement or reliable verification, use suitable RF
attenuation and proper test equipment.

## Hardware

- ESP32 Classic CYD 2.8" display board (ESP32-2432S028R)
- Built-in TFT display and resistive touchscreen
- ESP32 Wi-Fi radio used as the receive path for local RF capture

This is not a full SDR receiver, and it does not decode DATV video. It is best thought of as a compact RF visualization tool for local spectrum inspection.

## Project status

The firmware is intended for:

- visualizing local RF activity in the QO-100 uplink window
- checking signal presence, relative level, and occupancy
- exploring block spectrum behavior in a compact embedded display

It is not intended for:

- absolute power calibration in dBm
- exact frequency measurement without external verification
- receiving the 10 GHz downlink from QO-100
- decoding DATV or other digital payloads

## Repository layout

- `src/main.cpp` — screen UI, touch handling, serial commands, display loop
- `src/spectrum.cpp` / `src/spectrum.h` — FFT processing, signal averaging, burst suppression
- `src/esp_sdr.cpp` / `src/esp_sdr.h` — receive path and low-level capture logic
- `src/touch_input.cpp` / `src/touch_input.h` — touchscreen handling
- `src/splash_image.h` — RGB565 image data for the boot splash
- `tests/` — DSP and touch-related test code
- `scripts/spectrum_memory.ld` — linker script to protect the capture SRAM layout
- `platformio.ini` — PlatformIO configuration for the CYD board

## Build and flash

### Prerequisites

- PlatformIO Core or VS Code + PlatformIO extension
- USB cable connected to the ESP32 board
- Correct serial port selected for your machine

### Build

```bash
pio run -j 2
```

### Upload

```bash
pio run -t upload --upload-port COM34 -j 2
```

On Windows, replace `COM34` with the port assigned to your board. On Linux/macOS, use the corresponding serial device path.

### Monitor serial output

```bash
pio device monitor --port COM34 --baud 115200
```

## Touch and UI controls

- Tap the spectrum area to cycle trace rendering style
- Tap the waterfall area to cycle color palette
- Use the top-left checkbox to switch to full-spectrum mode
- Use the top-right checkbox to switch to full-waterfall mode
- Double-tap or tap gestures can toggle layout quickly
- BOOT button behavior is also implemented for alternate control shortcuts

## Serial commands

The firmware listens on serial at 115200 baud and supports commands such as:

```text
layout 0|1|2
mode 0-3
wf 0-4
burst 0|1
help
```

Meaning:

- `layout 0` = both spectrum + waterfall
- `layout 1` = full-spectrum mode
- `layout 2` = full-waterfall mode
- `mode 0-3` = dots / line / filled / gradient
- `wf 0-4` = classic / thermal / phosphor / gray / rainbow
- `burst 0|1` = disable or enable burst suppression

## How it works

The firmware runs a lightweight DSP pipeline:

1. The ESP32 captures raw I/Q samples.
2. A Hann-window FFT is applied to produce a spectrum.
3. The signal is averaged and mapped into dBFS values.
4. The display draws a trace and waterfall view on the CYD TFT.

The project uses a 512-point FFT and a 31.25 kHz bin spacing in the current configuration.

## Important notes

- The display is based on dBFS, not calibrated dBm.
- Signal levels are relative to the receiver chain and environment.
- The code includes burst suppression to smooth strong signals, but brief transmissions may still be missed.
- The frequency tuning is intended for local exploration, not production-grade metrology.
- If the display background appears white and the waterfall looks pink, check the TFT inversion configuration in `platformio.ini`.

## Safety

Use caution when testing antennas or RF paths. Do not connect a transmitter output directly to the receiver input without proper isolation and attenuation. This project is intended for receive-only experimentation.

## License

This project is distributed under the Apache-2.0 license. See
[LICENSE](docs/LICENSE) and [NOTICE](docs/NOTICE) for details.

## Acknowledgements

This project builds on ESP32 SDR research and related open-source work from the ESPARGOS ecosystem and the wider amateur radio / SDR community.

## Contributing

Pull requests and issue reports are welcome. If you improve DSP behavior, display rendering, or UX stability, please keep the project focused on practical, well-documented CYD-based spectrum viewing.
