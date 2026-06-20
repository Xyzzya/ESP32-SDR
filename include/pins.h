#pragma once
// ============================================================================
// ESP32-S3 SDR Receiver — Pin Definitions
// ============================================================================

// --- Si5351 I2C ---
#define PIN_SI5351_SDA      18
#define PIN_SI5351_SCL      8
#define SI5351_I2C_ADDR     0x60    // Default Si5351 address

// --- PCM1808 I2S (ESP32 is I2S Master, PCM1808 in Slave mode) ---
// MD0=L, MD1=L, FMT=L → Slave mode, I2S format, 256fs
// ESP32 pins → PCM1808 pins: SCKI(42), LRCK(41), BCK(38), OUT(40→ESP32 input)
#define PIN_I2S_MCLK        42      // Master clock    (ESP32 output → PCM1808 SCK)
#define PIN_I2S_LRCK        41      // Word select     (ESP32 output → PCM1808 LRCK)
#define PIN_I2S_DATA        40      // Serial data     (PCM1808 OUT → ESP32 input)
#define PIN_I2S_BCK         38      // Bit clock       (ESP32 output → PCM1808 BCK)

// --- I2S Parameters ---
#define I2S_SAMPLE_RATE     48000   // 48 kHz sample rate
#define I2S_MCLK_MULTIPLE   256     // PCM1808 slave mode: SCK = 256 × fs
#define I2S_BITS_PER_SAMPLE 32      // I2S slot width (PCM1808 outputs 24-bit data within each 32-bit slot)
#define I2S_NUM             I2S_NUM_0

// --- USB Audio Parameters ---
#define USB_AUDIO_SAMPLE_RATE   48000
#define USB_AUDIO_CHANNELS      2       // Stereo (I + Q)
#define USB_AUDIO_BIT_DEPTH     16      // 16-bit per sample for USB
#define USB_AUDIO_BYTES_PER_SAMPLE  2

// --- Si5351 Clock Assignments ---
// CLK0 + CLK1: Quadrature LO pair → 74HC4052 select lines (Gray code)
// CLK2: PCM1808 master clock (NOT used — ESP32 MCLK output instead)
#define SI5351_CLK0         0
#define SI5351_CLK1         1
#define SI5351_CLK_LO_I     SI5351_CLK0   // 0° (I channel LO)
#define SI5351_CLK_LO_Q     SI5351_CLK1   // 90° (Q channel LO)

// --- Frequency Limits ---
#define MIN_LO_FREQ_HZ       330000UL    // 330 kHz (VCO min 600 MHz / max divider 1800)
#define MAX_LO_FREQ_HZ       150000000UL // 150 MHz

// --- Default Frequencies ---
#define DEFAULT_LO_FREQ_HZ  11052000UL   // 11.052 MHz (test transmitter)
#define SI5351_XTAL_CORRECTION  0L  // Crystal frequency correction (tune with known frequency source)

// Default I2S digital gain (applied before 32→16 bit conversion)
#define DEFAULT_I2S_GAIN  1
