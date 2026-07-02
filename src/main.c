// ============================================================================
// ESP32-S3 SDR Receiver — Main Firmware (ESP-IDF)
//
// System overview:
//   1. Si5351 generates quadrature LO (CLK0 + CLK1) for 74HC4052 mux
//   2. PCM1808 ADC captures I/Q baseband audio via I2S (ESP32 is master)
//   3. USB Audio Class 2.0 (UAC2) streams I/Q to host as a USB microphone
//   4. USB CDC serial provides frequency control
//
// Data flow:
//   RF antenna → Tayloe mixer (74HC4052) → RC integrators → PCM1808 →
//   I2S → ESP32-S3 DMA → USB Audio (UAC2) → Host (HDSDR / SDR#)
//
// No virtual audio cables or Python bridge needed!
// HDSDR just selects "ESP32 SDR Receiver" as its audio input device.
// ============================================================================

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"

#include "pins.h"
#include "si5351_quad.h"
#include "i2s_audio.h"
#include "usb_audio.h"
#include "serial_cmd.h"


static const char *TAG = "sdr_main";

// ---- Audio Pipeline Buffers ----
#define AUDIO_CHUNK_FRAMES  48  // 1ms at 48kHz

static int32_t  i2s_raw_buf[AUDIO_CHUNK_FRAMES * 2];
static int16_t  usb_out_buf[AUDIO_CHUNK_FRAMES * 2];

// ---- Current State ----
static uint32_t current_freq_hz = DEFAULT_LO_FREQ_HZ;
static int      debug_level = 0;  // 0=off, 1=stats, 2=verbose

// ---- I2S Digital Gain ----
// Written from cmd_task (core 0), read from audio_task (core 1).
static uint32_t i2s_gain = DEFAULT_I2S_GAIN;

// ---- Test Tone Generator State ----
// Written from cmd_task (core 0), read from audio_task (core 1).
// Use explicit uint8_t with atomic builtins for cross-core visibility.
// tone_phase is only read/written by audio_task (core 1).
static uint8_t  tone_mode = 0;
static uint32_t tone_freq = 0;
static float    tone_phase = 0.0f;

// Chirp counter: set by on_freq_change, decremented by audio_task for audible feedback
static uint32_t chirp_frames = 0;

// ============================================================================
// Command Callbacks
// ============================================================================

static bool on_freq_change(uint32_t new_freq_hz) {
    if (new_freq_hz < MIN_LO_FREQ_HZ || new_freq_hz > MAX_LO_FREQ_HZ) {
        ESP_LOGW(TAG, "Frequency %lu Hz out of range", (unsigned long)new_freq_hz);
        return false;
    }
    bool ok = si5351_quad_set_freq(new_freq_hz);
    if (ok) {
        current_freq_hz = new_freq_hz;
        __atomic_store_n(&chirp_frames, 48, __ATOMIC_RELEASE);  // 1ms chirp @ 48kHz
        ESP_LOGI(TAG, "LO = %lu Hz", (unsigned long)current_freq_hz);
        if (debug_level >= 1) {
            uint32_t divider = current_freq_hz > 0 ? (900000000UL / current_freq_hz) & ~1UL : 0;
            if (divider < 6) divider = 6;
            char buf[64];
            int len = snprintf(buf, sizeof(buf), "\r\nLO:%luHz div~%lu\r\n",
                               (unsigned long)current_freq_hz, (unsigned long)divider);
            if (len > 0 && tud_cdc_connected()) {
                tud_cdc_write(buf, (uint32_t)len);
                tud_cdc_write_flush();
            }
        }
        return true;
    } else {
        ESP_LOGE(TAG, "Si5351 set_freq failed");
        cdc_write_str("\r\nERR: Si5351 set_freq failed\r\n");
        return false;
    }
}

static void on_output_enable(char output, bool enabled) {
    if (output == 'A') {
        bool ok = si5351_quad_enable(enabled);
        ESP_LOGI(TAG, "LO %s", enabled ? "ON" : "OFF");
        if (!ok) {
            ESP_LOGE(TAG, "LO %s FAILED (I2C error)", enabled ? "ON" : "OFF");
        }
    }
}

static void on_tone_cmd(uint32_t freq_hz) {
    if (freq_hz > 0) {
        __atomic_store_n(&tone_freq, freq_hz, __ATOMIC_RELEASE);
        __atomic_store_n(&tone_mode, (uint8_t)1, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Tone ON: %lu Hz", (unsigned long)freq_hz);
    } else {
        __atomic_store_n(&tone_mode, (uint8_t)0, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Tone OFF");
    }
}

static void on_debug_cmd(int level) {
    debug_level = level;
    if (level >= 1) {
        cdc_printf("\r\n--- Debug level %lu ---\r\n", (unsigned long)level);
    }
    ESP_LOGI(TAG, "Debug level: %d", level);
}

static void on_gain_cmd(uint32_t gain) {
    __atomic_store_n(&i2s_gain, gain, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "I2S gain: %lu", (unsigned long)gain);
    cdc_printf("\r\nGain: %lu\r\n", (unsigned long)gain);
}

// ============================================================================
// Audio Task — reads I2S and feeds USB audio
// ============================================================================

static void audio_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Audio pipeline task started");

    uint32_t last_tone_freq = 0;
    int64_t  stats_start_us = esp_timer_get_time();
    uint32_t stats_chunks = 0;
    uint32_t stats_drops = 0;
    bool     was_streaming = false;
    int64_t  acc_dc_i = 0, acc_peak_i = 0, acc_peak_q = 0;
    uint32_t acc_frames = 0;

    while (1) {
        bool streaming = usb_audio_is_streaming();

        // Detect streaming state transitions
        if (streaming && !was_streaming) {
            if (!si5351_quad_enable(true)) {
                ESP_LOGW(TAG, "Failed to enable LO on stream start");
            }
            i2s_audio_reset();
            stats_start_us = esp_timer_get_time();
            stats_chunks = 0;
            stats_drops = 0;
            ESP_LOGI(TAG, "==> Host started streaming");
            if (debug_level >= 1) cdc_write_str("\r\n==> Host started streaming\r\n");
        } else if (!streaming && was_streaming) {
            if (!si5351_quad_enable(false)) {
                ESP_LOGW(TAG, "Failed to disable LO on stream stop");
            }
            ESP_LOGI(TAG, "<== Host stopped streaming (chunks=%lu drops=%lu)",
                     (unsigned long)stats_chunks, (unsigned long)stats_drops);
            if (debug_level >= 1) {
                char stop_buf[64];
                int slen = snprintf(stop_buf, sizeof(stop_buf),
                    "\r\n<== Host stopped streaming (chunks=%lu drops=%lu)\r\n",
                    (unsigned long)stats_chunks, (unsigned long)stats_drops);
                if (slen > 0 && tud_cdc_connected()) {
                    tud_cdc_write(stop_buf, (uint32_t)slen);
                    tud_cdc_write_flush();
                }
            }
        }
        was_streaming = streaming;

        if (streaming) {
            // --- Frequency-change chirp (brief 1kHz tone for audible feedback) ---
            uint32_t chirp_left = __atomic_load_n(&chirp_frames, __ATOMIC_ACQUIRE);
            if (chirp_left > 0) {
                size_t n = chirp_left < AUDIO_CHUNK_FRAMES ? (size_t)chirp_left : AUDIO_CHUNK_FRAMES;
                float c_step = 2.0f * (float)M_PI * 1000.0f / (float)I2S_SAMPLE_RATE;
                float c_phase = 0.0f;
                for (size_t i = 0; i < n; i++) {
                    usb_out_buf[i * 2]     = (int16_t)(sinf(c_phase) * 16384.0f);
                    usb_out_buf[i * 2 + 1] = (int16_t)(sinf(c_phase) * 16384.0f);
                    c_phase += c_step;
                }
                // Zero-fill remaining frames
                for (size_t i = n; i < AUDIO_CHUNK_FRAMES; i++) {
                    usb_out_buf[i * 2]     = 0;
                    usb_out_buf[i * 2 + 1] = 0;
                }
                bool wrote = usb_audio_write(usb_out_buf, AUDIO_CHUNK_FRAMES);
                if (wrote) stats_chunks++; else stats_drops++;
                __atomic_store_n(&chirp_frames, chirp_left - (uint32_t)n, __ATOMIC_RELEASE);
                if (!wrote) vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (__atomic_load_n(&tone_mode, __ATOMIC_ACQUIRE) != 0) {
                // --- Test tone mode ---
                uint32_t freq = __atomic_load_n(&tone_freq, __ATOMIC_ACQUIRE);
                if (freq != last_tone_freq) {
                    tone_phase = 0.0f;
                    last_tone_freq = freq;
                    if (debug_level >= 1) {
                        cdc_printf("\r\nTONE ON: %lu Hz\r\n", (unsigned long)freq);
                    }
                }
                float phase_step = 2.0f * (float)M_PI * (float)freq / (float)I2S_SAMPLE_RATE;
                for (size_t i = 0; i < AUDIO_CHUNK_FRAMES; i++) {
                    usb_out_buf[i * 2]     = (int16_t)(sinf(tone_phase) * 16384.0f);
                    usb_out_buf[i * 2 + 1] = (int16_t)(cosf(tone_phase) * 16384.0f);
                    tone_phase += phase_step;
                }
                while (tone_phase > 2.0f * (float)M_PI) {
                    tone_phase -= 2.0f * (float)M_PI;
                }
                bool wrote = usb_audio_write(usb_out_buf, AUDIO_CHUNK_FRAMES);
                if (wrote) stats_chunks++; else stats_drops++;
                if (!wrote) vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                // --- Live I2S mode ---
                size_t frames = i2s_audio_read(i2s_raw_buf, AUDIO_CHUNK_FRAMES, pdMS_TO_TICKS(100));
                if (frames > 0) {
                    // Accumulate I2S signal stats from raw data (before gain)
                    for (size_t i = 0; i < frames; i++) {
                        int32_t s_i = i2s_raw_buf[i * 2];
                        int32_t s_q = i2s_raw_buf[i * 2 + 1];
                        acc_dc_i += s_i;
                        int32_t abs_i = s_i > 0 ? s_i : -s_i;
                        int32_t abs_q = s_q > 0 ? s_q : -s_q;
                        if (abs_i > acc_peak_i) acc_peak_i = abs_i;
                        if (abs_q > acc_peak_q) acc_peak_q = abs_q;
                    }
                    acc_frames += (uint32_t)frames;

                    // Apply digital gain in-place
                    uint32_t gain = __atomic_load_n(&i2s_gain, __ATOMIC_ACQUIRE);
                    for (size_t i = 0; i < frames * 2; i++) {
                        int64_t s = (int64_t)i2s_raw_buf[i] * gain;
                        if (s > 2147483647LL)  s = 2147483647LL;
                        if (s < -2147483648LL) s = -2147483648LL;
                        i2s_raw_buf[i] = (int32_t)s;
                    }
                    i2s_audio_convert_to_16bit(i2s_raw_buf, usb_out_buf, frames);
                    bool wrote = usb_audio_write(usb_out_buf, frames);
                    if (wrote) stats_chunks++; else stats_drops++;

                    // Yield to prevent TWDT starvation of the idle task
                    vTaskDelay(pdMS_TO_TICKS(1));

                    // --- Stats reporting every ~1 second ---
                    int64_t now_us = esp_timer_get_time();
                    int64_t elapsed_us = now_us - stats_start_us;
                    if (elapsed_us >= 1000000LL) {
                        float elapsed_s = (float)elapsed_us / 1000000.0f;
                        float rate_kfps = (float)stats_chunks * (float)AUDIO_CHUNK_FRAMES / elapsed_s / 1000.0f;

                        int64_t dc_i = acc_frames > 0 ? acc_dc_i / (int64_t)acc_frames : 0;
                        float peak_mv_i = (float)acc_peak_i / (float)0x800000 * 1500.0f;
                        float dc_mv_i   = (float)dc_i       / (float)0x800000 * 1500.0f;
                        float peak_mv_q = (float)acc_peak_q / (float)0x800000 * 1500.0f;

                        // Stats go to CDC only — UART TX radiates RF noise
                        // that appears as periodic spikes in the waterfall.

                        if (debug_level >= 1 && tud_cdc_connected()) {
                            char buf[120];
                            int len = snprintf(buf, sizeof(buf),
                                "\r\n%5.1fkfps  I-dc=%+.1fmV  I-pk=%.1fmV  Q-pk=%.1fmV  drops=%lu\r\n",
                                rate_kfps, dc_mv_i, peak_mv_i, peak_mv_q, (unsigned long)stats_drops);
                            if (len > 0) {
                                tud_cdc_write(buf, (uint32_t)len);
                                tud_cdc_write_flush();
                            }
                        }

                        // Verbose: dump a few raw samples to CDC
                        if (debug_level >= 2 && tud_cdc_connected()) {
                            char raw[80];
                            int rlen = snprintf(raw, sizeof(raw),
                                "  raw[0..3]: %08lX %08lX %08lX %08lX\r\n",
                                (uint32_t)i2s_raw_buf[0], (uint32_t)i2s_raw_buf[1],
                                (uint32_t)i2s_raw_buf[2], (uint32_t)i2s_raw_buf[3]);
                            if (rlen > 0) {
                                tud_cdc_write(raw, (uint32_t)rlen);
                                tud_cdc_write_flush();
                            }
                        }

                        stats_start_us = now_us;
                        stats_chunks = 0;
                        stats_drops = 0;
                        acc_dc_i = 0;
                        acc_peak_i = 0;
                        acc_peak_q = 0;
                        acc_frames = 0;
                    }
                } else {
                    ESP_LOGW(TAG, "I2S read timeout (100ms), resetting DMA");
                    i2s_audio_reset();
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
        } else {
            // Idle — host not streaming
            if (__atomic_load_n(&tone_mode, __ATOMIC_ACQUIRE) == 0) {
                i2s_audio_read(i2s_raw_buf, AUDIO_CHUNK_FRAMES, 1);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ============================================================================
// Command Task — processes serial commands from CDC
// ============================================================================

static void cmd_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Command task started");

    while (1) {
        serial_cmd_process();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}



// ============================================================================
// app_main — Entry Point
// ============================================================================

void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-S3 SDR Receiver");
    ESP_LOGI(TAG, "  Si5351 Quad + PCM1808 + USB Audio (UAC2)");
    ESP_LOGI(TAG, "========================================");

    // WiFi/BT disabled via sdkconfig — no RF noise from radios

    // ---- Initialize TinyUSB (CDC + UAC2) ----
    extern tusb_desc_device_t const desc_device;
    extern uint8_t const desc_configuration[];
    extern char const* string_desc_arr[];

    ESP_LOGI(TAG, "Initializing USB (CDC + UAC2)...");
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device,
        .string_descriptor = string_desc_arr,
        .string_descriptor_count = 6,
        .external_phy = false,
        .configuration_descriptor = desc_configuration,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB initialized");

    // ---- Initialize USB Audio ----
    usb_audio_init();

    // ---- Initialize Si5351 Quadrature LO ----
    ESP_LOGI(TAG, "Initializing Si5351...");
    if (si5351_quad_init(current_freq_hz)) {
        ESP_LOGI(TAG, "Si5351 OK, LO = %lu Hz", (unsigned long)current_freq_hz);
    } else {
        ESP_LOGE(TAG, "Si5351 FAILED! Check I2C wiring (SDA=%d, SCL=%d)",
                 PIN_SI5351_SDA, PIN_SI5351_SCL);
    }

    // ---- Initialize I2S for PCM1808 ----
    ESP_LOGI(TAG, "Initializing I2S...");
    if (i2s_audio_init()) {
        ESP_LOGI(TAG, "I2S OK");
    } else {
        ESP_LOGE(TAG, "I2S FAILED! Check PCM1808 wiring");
    }

    // ---- Initialize Serial Command Parser ----
    serial_cmd_init(on_freq_change, on_output_enable);
    serial_cmd_set_tone_callback(on_tone_cmd);
    serial_cmd_set_debug_callback(on_debug_cmd);
    serial_cmd_set_gain_callback(on_gain_cmd);
    serial_cmd_set_freq(current_freq_hz);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Commands: f<Hz>, F<kHz>, A 0/1, T<Hz>/T 0, D 0-2, G<n>, ?");
    ESP_LOGI(TAG, "Audio: Host selects 'ESP32 SDR Receiver' as input device");



    // ---- Launch FreeRTOS Tasks ----
    xTaskCreatePinnedToCore(audio_task,   "audio",   4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(cmd_task,     "cmd",     4096, NULL, 3, NULL, 0);

}
