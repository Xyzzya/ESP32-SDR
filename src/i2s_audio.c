// ============================================================================
// I2S Audio Capture — Implementation (ESP-IDF)
//
// Uses the ESP-IDF 5.x I2S standard mode driver.
// ESP32-S3 is I2S Master generating MCLK, BCK, LRCK.
// PCM1808 in slave mode provides serial audio data.
// ============================================================================

#include "i2s_audio.h"
#include "pins.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "i2s_audio";

// I2S channel handle
static i2s_chan_handle_t rx_channel = NULL;

bool i2s_audio_init(void) {
    // --- Create I2S channel (RX only) ---
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;    // 5ms at 48kHz
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %s", esp_err_to_name(err));
        return false;
    }

    // --- Configure standard mode (Philips I2S format) ---
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)PIN_I2S_MCLK,
            .bclk = (gpio_num_t)PIN_I2S_BCK,
            .ws   = (gpio_num_t)PIN_I2S_LRCK,
            .dout = GPIO_NUM_NC,
            .din  = (gpio_num_t)PIN_I2S_DATA,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(rx_channel, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2s_channel_enable(rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "I2S initialized: %dHz, 32-bit stereo, MCLK=%.3fMHz on GPIO%d",
             I2S_SAMPLE_RATE, I2S_SAMPLE_RATE * 256.0 / 1000000.0, PIN_I2S_MCLK);
    return true;
}

size_t i2s_audio_read(int32_t *buffer, size_t num_frames, uint32_t timeout_ticks) {
    if (!rx_channel || !buffer || num_frames == 0) return 0;

    size_t bytes_to_read = num_frames * 2 * sizeof(int32_t);
    size_t bytes_read = 0;

    esp_err_t err = i2s_channel_read(rx_channel, buffer, bytes_to_read, &bytes_read, timeout_ticks);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S read error (%s), %u bytes read", esp_err_to_name(err), (unsigned)bytes_read);
    }
    if (bytes_read == 0) {
        return 0;
    }
    size_t frames = bytes_read / (2 * sizeof(int32_t));
    size_t total_samples = frames * 2;
    for (size_t i = 0; i < total_samples; i++) {
        buffer[i] >>= 8;
    }
    return frames;
}

void i2s_audio_convert_to_16bit(const int32_t *in, int16_t *out, size_t frames) {
    size_t total_samples = frames * 2;
    for (size_t i = 0; i < total_samples; i++) {
        out[i] = (int16_t)(in[i] >> 8);
    }
}

void i2s_audio_reset(void) {
    if (!rx_channel) return;
    ESP_LOGI(TAG, "Resetting I2S DMA");
    i2s_channel_disable(rx_channel);
    i2s_channel_enable(rx_channel);
}
