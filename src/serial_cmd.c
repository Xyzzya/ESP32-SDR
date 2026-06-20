// ============================================================================
// Serial Command Parser — ESP-IDF / TinyUSB CDC Implementation
//
// Reads line-by-line from TinyUSB CDC interface.
// Parses the SDRshield command protocol and dispatches to callbacks.
//
// The 'S' (stream) command is removed since audio streaming is now
// controlled by the host via the USB Audio Class interface.
// ============================================================================

#include "serial_cmd.h"
#include "tusb.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "serial_cmd";

static freq_change_cb_t    _on_freq_change = NULL;
static output_enable_cb_t  _on_output_enable = NULL;
static tone_cmd_cb_t       _on_tone = NULL;
static debug_cmd_cb_t      _on_debug = NULL;
static gain_cmd_cb_t       _on_gain = NULL;

static uint32_t _last_freq_hz = 0;

static char cmd_buffer[64];
static uint8_t cmd_pos = 0;

typedef enum {
    CMD_SRC_CDC = 0,
    CMD_SRC_UART
} cmd_source_t;

static cmd_source_t _cmd_source = CMD_SRC_CDC;

// --- I/O helpers ---

void cdc_write_str(const char *str) {
    if (tud_cdc_connected()) {
        tud_cdc_write_str(str);
        tud_cdc_write_flush();
    }
}

void cdc_printf(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    cdc_write_str(buf);
}

// --- Command processing ---

static void process_line(const char *line) {
    if (!line || line[0] == '\0') return;

    // Echo received command back to CDC for diagnostics
    cdc_write_str("CMD:");
    cdc_write_str(line);
    cdc_write_str("\r\n");

    // Omni-Rig / EasySDR compatibility: "FREQ,<Hz>"
    if (strncmp(line, "FREQ,", 5) == 0) {
        long val = atol(line + 5);
        if (val > 0) {
            uint32_t freq = (uint32_t)val;
            if (_on_freq_change) {
                if (_on_freq_change(freq)) {
                    _last_freq_hz = freq;
                    serial_cmd_report_freq(freq);
                }
            }
        }
        return;
    }

    // EasySDR-style version query
    if (strncmp(line, "VER", 3) == 0) {
        cdc_printf("VER,ESP32-S3 SDR V1.0\r\n");
        return;
    }

    char cmd = line[0];
    const char *arg = &line[1];

    // Skip whitespace after command character
    while (*arg == ' ') arg++;

    switch (cmd) {
        case 'f': {
            // f <freq_hz> — Set frequency in Hz
            long val = atol(arg);
            if (val > 0) {
                uint32_t freq = (uint32_t)val;
                if (_on_freq_change) {
                    if (_on_freq_change(freq)) {
                        _last_freq_hz = freq;
                        serial_cmd_report_freq(freq);
                    }
                }
            }
            break;
        }
        case 'F': {
            // F <freq_khz> — Set frequency in kHz
            long val = atol(arg);
            if (val > 0) {
                uint32_t freq_khz = (uint32_t)val;
                if (_on_freq_change) {
                    uint32_t freq_hz = freq_khz * 1000UL;
                    if (_on_freq_change(freq_hz)) {
                        _last_freq_hz = freq_hz;
                        serial_cmd_report_freq(freq_hz);
                    }
                }
            }
            break;
        }
        case 'A': {
            // A 0 or A 1 — Enable/disable LO
            if (arg[0] == '0' || arg[0] == '1') {
                bool enable = (arg[0] == '1');
                if (_on_output_enable) {
                    _on_output_enable('A', enable);
                }
                cdc_printf("LO %s\r\n", enable ? "ON" : "OFF");
            } else {
                cdc_write_str("ERR: A 0 or A 1 expected\r\n");
            }
            break;
        }
        case '?': {
            // Query — report current frequency
            if (_last_freq_hz > 0) {
                serial_cmd_report_freq(_last_freq_hz);
            } else {
                cdc_write_str("FREQ: unknown\r\n");
            }
            break;
        }
        case 'D': {
            // D 0/1/2 — Set debug level
            int level = (int)atol(arg);
            if (level >= 0 && level <= 2) {
                if (_on_debug) _on_debug(level);
                cdc_printf("DBG:%d\r\n", level);
            } else {
                cdc_write_str("ERR: D 0,1,2 expected\r\n");
            }
            break;
        }
        case 'T': {
            // T <freq_hz> — Enable test tone, T 0 — Disable
            long val = atol(arg);
            if (val < 0) val = 0;
            uint32_t freq = (uint32_t)val;
            if (_on_tone) {
                _on_tone(freq);
            }
            if (freq > 0) {
                cdc_printf("TONE:%lu Hz\r\n", (unsigned long)freq);
            } else {
                cdc_write_str("TONE:OFF\r\n");
            }
            break;
        }
        case 'G': {
            // G <n> — Set I2S digital gain (1..65536)
            long val = atol(arg);
            if (val < 1) val = 1;
            if (val > 65536) val = 65536;
            uint32_t gain = (uint32_t)val;
            if (_on_gain) _on_gain(gain);
            cdc_printf("GAIN:%lu\r\n", (unsigned long)gain);
            break;
        }
        default:
            cdc_write_str("Unknown cmd: ");
            cdc_write_str(line);
            cdc_write_str("\r\n");
            break;
    }
}

// --- Public API ---

void serial_cmd_init(freq_change_cb_t on_freq_change,
                     output_enable_cb_t on_output_enable) {
    _on_freq_change = on_freq_change;
    _on_output_enable = on_output_enable;
    cmd_pos = 0;
    memset(cmd_buffer, 0, sizeof(cmd_buffer));

    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_config_t uart_cfg = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        };
        esp_err_t err = uart_param_config(UART_NUM_0, &uart_cfg);
        if (err == ESP_OK) {
            err = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "UART0 driver installed — commands accepted");
        } else {
            ESP_LOGW(TAG, "UART0 driver install failed (%s), CDC-only mode", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Serial command parser initialized");
}

void serial_cmd_set_tone_callback(tone_cmd_cb_t on_tone) {
    _on_tone = on_tone;
}

void serial_cmd_set_debug_callback(debug_cmd_cb_t on_debug) {
    _on_debug = on_debug;
}

void serial_cmd_set_gain_callback(gain_cmd_cb_t on_gain) {
    _on_gain = on_gain;
}

void serial_cmd_set_freq(uint32_t freq_hz) {
    _last_freq_hz = freq_hz;
}

static bool feed_byte(char ch, cmd_source_t source) {
    if (ch == '\r' || ch == '\n') {
        if (cmd_pos > 0) {
            cmd_buffer[cmd_pos] = '\0';
            process_line(cmd_buffer);
            cmd_pos = 0;
            _cmd_source = CMD_SRC_CDC;
        }
        return true;
    }

    if (cmd_pos == 0) {
        _cmd_source = source;
    }

    if (cmd_pos < sizeof(cmd_buffer) - 1) {
        cmd_buffer[cmd_pos++] = ch;
    } else {
        if (cmd_pos == sizeof(cmd_buffer) - 1) {
            cdc_write_str("ERR: command too long\r\n");
        }
        if (source == CMD_SRC_CDC) {
            while (tud_cdc_available()) {
                char skip = (char)tud_cdc_read_char();
                if (skip == '\r' || skip == '\n') break;
            }
        } else {
            uint8_t skip;
            while (uart_read_bytes(UART_NUM_0, &skip, 1, 0) > 0) {
                if (skip == '\r' || skip == '\n') break;
            }
        }
        cmd_pos = 0;
        _cmd_source = CMD_SRC_CDC;
    }
    return true;
}

void serial_cmd_process(void) {
    // Read from UART0 (USB-UART bridge on devkit — PlatformIO serial monitor)
    // Cap reads to prevent starving other tasks during sustained input.
    if (uart_is_driver_installed(UART_NUM_0)) {
        uint8_t uart_ch;
        int uart_count = 0;
        while (uart_count < 32 && uart_read_bytes(UART_NUM_0, &uart_ch, 1, 0) > 0) {
            feed_byte((char)uart_ch, CMD_SRC_UART);
            uart_count++;
        }
    }

    // Read from USB CDC (TinyUSB — separate Virtual COM port on native USB)
    while (tud_cdc_available()) {
        feed_byte((char)tud_cdc_read_char(), CMD_SRC_CDC);
    }
}

void serial_cmd_report_freq(uint32_t freq_hz) {
    cdc_printf("FREQ:%lu\r\n", (unsigned long)freq_hz);
}
