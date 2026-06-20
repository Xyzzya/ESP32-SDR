// ============================================================================
// USB Audio Streaming — UAC2 (USB Audio Class 2.0)
//
// Streams I/Q audio to the host as a standard USB microphone.
// Audio format: 48 kHz, 16-bit, stereo (Left=I, Right=Q)
//
// TinyUSB 0.19.0 provides its own internal TX FIFO. We call
// tud_audio_write() directly — no manual ring buffer needed.
// ============================================================================

#include "usb_audio.h"
#include "pins.h"
#include "tusb.h"
#include "class/audio/audio.h"
#include "class/audio/audio_device.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "usb_audio";

// Audio streaming state — controlled by the host selecting alt setting 1
static bool _host_streaming = false;

bool usb_audio_init(void) {
    __atomic_store_n(&_host_streaming, false, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "USB Audio (UAC2) init: 48kHz stereo 16-bit microphone");
    ESP_LOGI(TAG, "Host selects 'ESP32 SDR Receiver' as audio input");
    return true;
}

// Write audio samples directly to the TinyUSB internal TX FIFO.
// Returns true if any data was queued.
bool usb_audio_write(const int16_t *samples, size_t frames) {
    if (!__atomic_load_n(&_host_streaming, __ATOMIC_ACQUIRE) || !samples || frames == 0) return false;
    return tud_audio_write(samples, (uint16_t)(frames * USB_AUDIO_CHANNELS * USB_AUDIO_BYTES_PER_SAMPLE)) > 0;
}

void usb_audio_task(void) {
    // TinyUSB task is handled internally by esp_tinyusb
}

bool usb_audio_is_streaming(void) {
    return __atomic_load_n(&_host_streaming, __ATOMIC_ACQUIRE);
}

bool usb_audio_is_text_ok(void) {
    return true;  // CDC and Audio are on separate interfaces
}

// ============================================================================
// TinyUSB Audio Callbacks
// ============================================================================

// Called when the host selects an alternate interface setting.
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);

    ESP_LOGI(TAG, "Audio interface %d alt setting %d", itf, alt);

    if (alt == 1) {
        __atomic_store_n(&_host_streaming, true, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Host started audio capture");
    } else {
        __atomic_store_n(&_host_streaming, false, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Host stopped audio capture");
    }

    return true;
}

// Clock source and control request handling
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport; (void)p_request;
    return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
        static uint32_t sample_rate = USB_AUDIO_SAMPLE_RATE;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t*)p_request, &sample_rate, sizeof(sample_rate));
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;

    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    // Clock Source entity (ID=1)
    if (entityID == 1) {
        switch (ctrlSel) {
            case AUDIO_CS_CTRL_SAM_FREQ: {
                if (p_request->bRequest == AUDIO_CS_REQ_CUR) {
                    static uint32_t freq = 48000;
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t*)p_request,
                        &freq, sizeof(freq));
                } else if (p_request->bRequest == AUDIO_CS_REQ_RANGE) {
                    static struct {
                        uint16_t wNumSubRanges;
                        uint32_t dMIN;
                        uint32_t dMAX;
                        uint32_t dRES;
                    } __attribute__((packed)) range = {
                        .wNumSubRanges = 1,
                        .dMIN = 48000,
                        .dMAX = 48000,
                        .dRES = 0,
                    };
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t*)p_request,
                        &range, sizeof(range));
                }
                break;
            }
            case AUDIO_CS_CTRL_CLK_VALID: {
                static uint8_t valid = 1;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t*)p_request,
                    &valid, sizeof(valid));
            }
            default:
                break;
        }
    }

    return false;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport; (void)p_request; (void)buf;
    return true;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport; (void)p_request; (void)buf;
    return true;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    (void)rhport; (void)p_request; (void)buf;
    return true;
}
