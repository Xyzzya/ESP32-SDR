# ESP32-S3 SDR Receiver

A Software Defined Radio receiver firmware for the ESP32-S3 microcontroller. Uses a Si5351 quadrature local oscillator, 74HC4052 Tayloe mixer, and PCM1808 I2S ADC. Streams digitized I/Q baseband audio to a host PC over USB as a standard USB Audio Class 2.0 (UAC2) microphone — no virtual audio cables or Python bridges required.

> **Architecture note:** The 74HC4052 Tayloe quadrature sampling detector typically requires a switching clock at 4× the receive frequency. However, in this design, the Si5351 uses its phase offset registers to natively generate two 90° shifted clocks (CLK0 and CLK1) at the **1× receive frequency**. These directly drive the S0/S1 select lines, generating a 2-bit Gray code (00→01→11→10) that cycles the multiplexer through all four phases once per RF cycle.

## System Overview

```
RF Antenna
    │
    ▼
Tayloe Mixer (74HC4052)
    │   ◄── Si5351 Quadrature LO (CLK0 @ 0°, CLK1 @ 90°)
    ▼
RC Integrators
    │
    ▼
PCM1808 ADC ──I2S──► ESP32-S3 ──USB──► Host PC
                                       │
                        USB CDC (serial commands)
                            ◄──────────────┘
```

**Signal chain:** RF antenna → Tayloe mixer (74HC4052) → RC integrators → PCM1808 ADC → I2S (ESP32 master) → ESP32-S3 DMA → USB Audio UAC2 → Host PC (HDSDR / SDR#)

**Control path:** Host PC (Web Serial) → USB CDC → ESP32-S3 → I2C → Si5351 (LO frequency)

The ESP32-S3 appears as a composite USB device combining a UAC2 microphone and a CDC serial port. SDR software simply selects "ESP32 SDR Receiver" as the audio input and tunes via serial commands.

## Hardware Requirements

- **ESP32-S3-DevKitC-1** (N16R8 variant: 16 MB flash, 8 MB octal PSRAM)
- **Si5351A** clock generator module (I2C, default address 0x60)
- **PCM1808** stereo ADC module (configured for slave mode, I2S format, 256fs)
- **74HC4052** dual 4-channel analog multiplexer (Tayloe mixer core)
- **RC integrators** on the mixer output (e.g., 1k + 10nF per channel), in this case an NE5532 on a differential amp mode 
- **25 MHz crystal** on the Si5351 module
- USB-C cable for power and data

### Pin Connections

#### Si5351 (I2C)

| Si5351 Pin | ESP32-S3 GPIO | Description |
|-----------|---------------|-------------|
| SDA       | 18            | I2C data    |
| SCL       | 8             | I2C clock   |
| CLK0      | 74HC4052 S0   | I-channel LO (0°) |
| CLK1      | 74HC4052 S1   | Q-channel LO (90°) |

#### PCM1808 (I2S, Slave Mode)

Set PCM1808 mode pins: MD0=L, MD1=L, FMT=L (slave mode, I2S format, 256fs).

| PCM1808 Pin | ESP32-S3 GPIO | Direction       |
|-------------|---------------|-----------------|
| SCK (MCLK)  | 42            | ESP32 → PCM1808 |
| LRCK        | 41            | ESP32 → PCM1808 |
| BCK         | 38            | ESP32 → PCM1808 |
| DOUT        | 40            | PCM1808 → ESP32 |

## Software Prerequisites

1. [PlatformIO IDE](https://platformio.org/) (VS Code extension) or PlatformIO Core CLI
2. ESP-IDF 5.1.2 (PlatformIO handles this automatically)
3. USB driver for ESP32-S3 (Windows: [WinUSB via Zadig](https://zadig.akeo.ie/) or [Espressif USB CDC driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-serial-jtag-console.html))

## Build & Flash

```bash
# Clone the repository
git clone <repo-url>
cd SDR

# Install dependencies and build
pio run

# Flash to ESP32-S3 (921600 baud)
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor
```

On first flash, the ESP32-S3 will connect via the USB-UART bridge (GPIO19/20). After flashing, it will enumerate as a composite USB device (CDC serial + audio microphone) on the native USB port.

> **Note:** The build automatically includes TinyUSB with UAC2 support. No manual TinyUSB configuration is needed — `platformio.ini` injects the required compile flags (`CFG_TUD_AUDIO=1`, `CFG_TUSB_MCU=OPT_MCU_ESP32S3`). The `check_wifi.py` pre-build script also runs automatically to patch ESP-IDF's WiFi Kconfig to default off, ensuring minimal RF noise for SDR reception.

## Usage

1. **Connect hardware** as per the pin table above
2. **Plug in USB** — the ESP32-S3 will enumerate as "ESP32 SDR Receiver" (a USB microphone + CDC serial port)
3. **Open SDR software** (HDSDR, SDR#, etc.)
4. **Select input device:** "ESP32 SDR Receiver" or the corresponding microphone device
5. **Set audio format:** 48 kHz sample rate, 16-bit, stereo (left channel = I, right channel = Q)
6. **Tune via serial** using any terminal program or the web serial control interface

### HDSDR Configuration

- Soundcard → Input: select "ESP32 SDR Receiver"
- Bandwidth: set to match 48 kHz input sample rate (typically 48000 Hz output sample rate in HDSDR)
- LO frequency: controlled via serial commands (see below)
- CAT control: use Omni-Rig with the provided INI file for seamless tuning (see Omni-Rig Integration)

## Serial Command Reference

The firmware uses an SDRshield-compatible command protocol over the USB CDC serial port (any terminal at 115200 baud, CR/LF terminated). Commands are also accepted via UART0 (the built-in USB-UART bridge on the devkit), making the PlatformIO serial monitor (`pio device monitor`) a convenient control interface.

### Frequency & LO Control

| Command      | Example           | Description                                          |
|--------------|-------------------|------------------------------------------------------|
| `f <Hz>`     | `f 14100000`      | Set LO frequency in Hz                               |
| `F <kHz>`    | `F 14100`         | Set LO frequency in kHz (×1000 for Hz)               |
| `FREQ,<Hz>`  | `FREQ,14100000`   | Omni-Rig / EasySDR compatible frequency set          |
| `?`          | `?`               | Query current LO frequency → `FREQ:<Hz>`             |
| `A 0`        | `A 0`             | Disable quadrature LO outputs                        |
| `A 1`        | `A 1`             | Enable quadrature LO outputs                         |

> **Note:** LO outputs auto-enable when the host starts USB audio streaming and auto-disable when the host stops. Manual `A 0`/`A 1` overrides during an active stream are also respected.

**Frequency range:** 333 kHz to 150 MHz (receive frequency). The Si5351 generates quadrature signals natively at the exact receive frequency. The default LO frequency is 11.059 MHz.

**Example session:**
```
> f 14100000
FREQ:14100000
> A 1
LO ON
> ?
FREQ:14100000
```

### Test Tone Generator

A built-in sine/cosine tone generator replaces live I/Q samples for testing and calibration. When active, the I channel carries a sine wave and the Q channel carries a cosine wave at the specified frequency — useful for verifying the USB audio pipeline and SDR software demodulation without an RF signal.

| Command    | Example     | Description                                          |
|------------|-------------|------------------------------------------------------|
| `T <Hz>`   | `T 1000`    | Enable test tone at specified Hz (sine on I, cosine on Q) |
| `T 0`      | `T 0`       | Disable test tone, resume live I2S input             |

```
> T 1000
TONE:1000 Hz
> T 0
TONE:OFF
```

### Debug & Diagnostics

| Command  | Example | Description                                          |
|----------|---------|------------------------------------------------------|
| `D 0`    | `D 0`   | Debug off (no CDC output beyond command responses)   |
| `D 1`    | `D 1`   | Stats mode — I2S throughput & signal levels every ~1s |
| `D 2`    | `D 2`   | Verbose mode — includes raw I2S sample hex dump      |
| `G <n>`  | `G 256` | Set I2S digital gain (1..65536, default 1)           |
| `VER`    | `VER`   | Firmware version query → `VER,ESP32-S3 SDR V1.0`     |

At debug level 1, the device streams per-second I2S diagnostics to CDC:
```
  48.0kfps  I-dc=+0.5mV  I-pk=12.3mV  Q-pk=11.8mV  drops=0
```
- **kfps** — effective sample rate (kilo-frames per second)
- **I-dc** — DC offset on I channel (mV, relative to PCM1808 Vcc/2 = 1.5V)
- **I-pk / Q-pk** — peak signal amplitude (mV)
- **drops** — USB buffer overflow count (should be zero)

At debug level 2, four raw I2S samples (32-bit hex) are also printed each second for low-level signal inspection.

Audio streaming starts automatically when the host PC opens the USB audio interface. No explicit start/stop command is needed.

## Omni-Rig Integration

For seamless CAT control with HDSDR and other SDR software, an Omni-Rig INI file is provided.

1. Copy `omni-rig/ESP32_S3_SDR.ini` to `%APPDATA%\Afreet\Rigs\`
2. Install and start [Omni-Rig](http://www.dxatlas.com/OmniRig/)
3. Select RIG1 → Rig Type: **"ESP32-S3 SDR"**, set COM port and **115200 baud**
4. In HDSDR: Options → CAT to Omni-Rig → select RIG1

Omni-Rig sends `FREQ,<Hz>` commands which the firmware handles identically to `f <Hz>`. Frequency readback is not supported natively by Omni-Rig, but the device echoes every frequency change on CDC.

## Test Scripts

Two Python test utilities are included for validation without SDR software:

### `test_usb.py` — CDC + Audio Capture Test

Automated end-to-end test that validates both the CDC serial interface and USB audio I/Q capture. Requires `pyserial`, `sounddevice`, and `numpy`.

```bash
# Auto-detect ESP32-S3, run CDC + audio FFT test
python test_usb.py

# CDC commands only (no audio capture)
python test_usb.py --cdc-only

# Use 2 kHz test tone, specify port manually
python test_usb.py -f 2000 -p COM8

# List available serial ports and audio devices
python test_usb.py -l
```

The script:
- Sends `?`, `F`, and `T` commands via CDC and verifies responses
- Enables a test tone, captures USB audio, and runs FFT analysis
- Expects the tone peak within ±5 Hz of the commanded frequency

### `test_vac.py` — Virtual Audio Cable Test Signal

A square wave generator for testing SDR virtual audio cable (VAC) setups. Requires `sounddevice` and `numpy`.

```bash
# Auto-detect VAC output device
python test_vac.py

# Specify output device by ID
python test_vac.py -d 3
```

Generates a 440 Hz square wave on all output channels. Useful for verifying that your VAC routing is correctly feeding audio into SDR software before connecting the real ESP32-S3 receiver.

## Project Structure

```
.
├── platformio.ini             # PlatformIO build configuration
├── CMakeLists.txt             # ESP-IDF project entry point
├── sdkconfig.defaults         # ESP-IDF SDK configuration overrides
├── dependencies.lock          # Managed component versions
├── check_wifi.py              # Pre-build script — patches ESP-IDF to disable WiFi
├── src/
│   ├── main.c                 # Entry point (app_main), task creation, command callbacks
│   ├── si5351_quad.c          # Si5351 I2C driver — quadrature LO generation
│   ├── i2s_audio.c            # I2S capture driver — PCM1808 master mode
│   ├── usb_audio.c            # USB Audio UAC2 streaming — ring buffer + callbacks
│   ├── usb_descriptors.c      # USB composite device descriptors (CDC + UAC2)
│   ├── serial_cmd.c           # Serial command parser (CDC + UART0)
│   ├── CMakeLists.txt         # Component registration + TinyUSB UAC2 config injection
│   └── idf_component.yml      # ESP-IDF managed component dependencies
├── include/
│   ├── pins.h                 # Pin definitions, constants, default frequencies
│   ├── si5351_quad.h          # Si5351 public API
│   ├── i2s_audio.h            # I2S capture public API
│   ├── usb_audio.h            # USB audio streaming public API
│   └── serial_cmd.h           # Serial command parser public API
├── omni-rig/
│   └── ESP32_S3_SDR.ini       # Omni-Rig CAT control definition file
├── test_vac.py                # Square wave generator for VAC testing
└── test_usb.py                # Automated CDC + audio capture test suite
```

### Module Responsibilities

| Module | Lines | Role |
|--------|-------|------|
| `main.c` | 405 | Entry point. Initializes all subsystems, creates FreeRTOS audio and command tasks. Manages test tone generator, I2S gain, chirp feedback, and per-second I2S diagnostics. |
| `si5351_quad.c` | 290 | Direct register-level I2C control of Si5351. PLL feedback divider and multisynth divider calculations. Phase offset for quadrature. |
| `i2s_audio.c` | 88 | ESP32-S3 I2S master driver with MCLK output. Reads 32-bit stereo samples from PCM1808 via DMA and converts to 16-bit. |
| `usb_audio.c` | 231 | UAC2 microphone streaming. Ring buffer between I2S task and TinyUSB isochronous IN transfers. Handles audio control requests. |
| `usb_descriptors.c` | 202 | USB composite device descriptors for CDC + UAC2. VID:PID = 0x303A:0x8001. |
| `serial_cmd.c` | 296 | Line-terminated command parser over CDC and UART0 serial. Dispatches to frequency-change, output-enable, tone, debug, and gain callbacks. Supports Omni-Rig `FREQ,<Hz>` format. |

## FreeRTOS Tasks

| Task    | Core | Priority | Stack | Description |
|---------|------|----------|-------|-------------|
| `audio` | 1    | 5        | 4096  | Reads I2S DMA or generates test tones, applies digital gain, converts to 16-bit, and feeds the USB audio ring buffer. Runs at 48-sample chunks (1 ms at 48 kHz). Manages auto-LO on/off on stream state transitions, chirp audio on frequency change, and per-second I2S diagnostics output. |
| `cmd`   | 0    | 3        | 4096  | Polls CDC serial and UART0 for incoming commands, dispatches to callbacks. Processes at ~100 Hz (10 ms intervals). |

The audio task runs on core 1 for low latency (core 0 handles USB and the command task). When the host is not streaming and test tone is off, the audio task drains the I2S DMA buffer to prevent overflow and sleeps for 10 ms.

## Configuration

### Build Flags (`platformio.ini`)

| Flag | Purpose |
|------|---------|
| `CFG_TUSB_MCU=OPT_MCU_ESP32S3` | Target MCU for TinyUSB |
| `CFG_TUD_AUDIO=1` | Enable USB Audio Class 2.0 in TinyUSB |

### SDK Configuration (`sdkconfig.defaults`)

| Setting | Value | Purpose |
|---------|-------|---------|
| `CONFIG_TINYUSB_CDC_ENABLED` | y | Enable CDC serial |
| `CONFIG_TINYUSB_CDC_COUNT` | 1 | One CDC port |
| `CONFIG_FREERTOS_HZ` | 1000 | 1 kHz tick for timing |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | 10 | Task watchdog timeout |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | y | 16 MB flash |
| `CONFIG_SPIRAM_MODE_OCT` | y | Octal PSRAM mode |
| `CONFIG_LOG_DEFAULT_LEVEL_INFO` | y | Info-level logging |

### Adjustable Constants (`include/pins.h`)

| Define | Default | Description |
|--------|---------|-------------|
| `DEFAULT_LO_FREQ_HZ` | 11059000 | Startup receive frequency (11.059 MHz beacon). |
| `SI5351_XTAL_CORRECTION` | 0L | Crystal frequency correction in 0.01 Hz units (calibrate per-module) |
| `I2S_SAMPLE_RATE` | 48000 | Audio sample rate |
| `USB_AUDIO_SAMPLE_RATE` | 48000 | USB audio sample rate |
| `DEFAULT_I2S_GAIN` | 1 | I2S digital gain multiplier (1..65536, adjustable at runtime via `G` command) |

## How It Works

The receiver uses a **Tayloe quadrature sampling detector** — a proven design for direct-conversion SDRs. The Si5351 generates two clock signals at the desired LO frequency with a 90-degree phase offset. These drive the 74HC4052 analog multiplexer, which acts as a commutating mixer. The multiplexer output passes through RC low-pass filters (integrators) to the PCM1808 stereo ADC, producing I (in-phase) and Q (quadrature) baseband signals.

The ESP32-S3 reads the PCM1808 as an I2S master at 48 kHz and exposes the I/Q stream as a standard USB Audio Class 2.0 microphone. The host PC treats it like any other audio input device — no special drivers or software bridges needed. Frequency control uses a separate CDC serial interface on the same USB cable, supporting standard SDRshield-compatible commands as well as Omni-Rig / EasySDR `FREQ,<Hz>` format.

The critical quadrature relationship is achieved through the Si5351's phase offset register: CLK1 is programmed with a phase offset equal to the multisynth divider value, producing a 90-degree delay relative to CLK0 at the output frequency. This makes CLK0 (I channel) lead CLK1 (Q channel) by 90°.

The Si5351 output frequency is set to exactly the **1× receive frequency** because the 74HC4052's two select lines (S0=CLK0, S1=CLK1) natively form a Gray code that cycles through all four multiplexer states once per RF period, producing the required 0°/90°/180°/270° commutation without a 4× multiplier.

### Key Behaviors

- **Auto LO on/off:** The quadrature LO outputs automatically enable when the host opens the USB audio streaming interface and disable when the host stops streaming. This prevents unnecessary RF emission and power consumption when the receiver is not in use.
- **Frequency change chirp:** A brief 1 ms 1 kHz tone is injected into the USB audio stream whenever the LO frequency is changed, providing audible confirmation of tuning changes in the SDR waterfall.
- **Built-in test tone:** The `T` command replaces live I/Q with a pure sine/cosine pair, enabling end-to-end testing of the USB audio pipeline and SDR software demodulation without an antenna or RF signal generator.
- **Per-second I2S diagnostics:** At debug level ≥ 1, the device streams throughput (kfps), DC offset, peak I/Q amplitude (mV), and buffer drop count over CDC for real-time signal health monitoring.

## Troubleshooting

- **Si5351 not detected:** Verify I2C wiring (SDA=GPIO18, SCL=GPIO8). Check that the module is powered and the I2C address is 0x60.
- **No USB audio device appears:** Ensure `CFG_TUD_AUDIO=1` is in build flags and TinyUSB managed components are installed (`pio run` handles this).
- **No audio in SDR software:** Confirm the host has selected the correct audio input device (48 kHz, 16-bit, stereo). Check that the USB audio interface is at alt setting 1 (streaming active). Verify that another application hasn't already claimed exclusive access to the audio device.
- **I2S errors:** Verify PCM1808 mode pins are set for slave mode (MD0=L, MD1=L, FMT=L) and all I2S connections match the pin table.
- **Frequency accuracy:** Adjust `SI5351_XTAL_CORRECTION` in `include/pins.h` to calibrate the Si5351 crystal. The correction is in 0.01 Hz units relative to 25 MHz. Use a known frequency source (e.g., WWV at 10 MHz or a local AM broadcast station) and fine-tune until the carrier appears at the expected frequency in your SDR software.
- **WiFi re-enabled by ESP-IDF Kconfig:** The `check_wifi.py` pre-build script (configured in `platformio.ini` as `extra_scripts`) automatically patches the ESP-IDF WiFi Kconfig to default WiFi off. This ensures no RF noise from the WiFi/BT radios. If you see WiFi-related build warnings or if the build fails due to Kconfig issues, check that the patch was applied (`check_wifi.py: PATCHED ...` appears in build output). Reinstalling the ESP-IDF framework (`pio pkg update`) may require the patch to be re-applied on the next build.
- **USB audio capture fails with Python:** `test_usb.py` may time out on Windows due to WASAPI exclusive-mode limitations with UAC2 devices. This does not indicate a firmware problem — use HDSDR or SDR# for full audio testing, as they use the standard Windows audio APIs (MME/WDM-KS) which handle UAC2 correctly.
- **Buffer drops (drops > 0 in stats):** Indicates the USB host is not consuming audio data fast enough. Try reducing host CPU load, closing other USB devices on the same controller, or using a direct USB port (not a hub).

## Notes

Works with strong signals; will struggle on weak ones. If you want better performance, I suggest using FST3253 for the switcher/mux, a better RF frontend, better op-amp, and better biasing. I've done the RF side on an etched copper-clad board.

Software may be bug-laden. I did my best to diagnose and debug.

LLMs used: DeepSeek V4 Pro, Gemini 3.1 Pro, and Claude Opus 4.6

## Demo

1 minute demo on NHK World Radio 11.815MHz (25m band)

https://github.com/user-attachments/assets/525b316a-7277-4892-81d7-3af65f93265f





## License

This project is licensed under the MIT License.

```
MIT License

Copyright (c) 2026

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
