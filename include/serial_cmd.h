#pragma once
// ============================================================================
// Serial Command Parser (ESP-IDF / TinyUSB CDC)
//
// Handles frequency control commands over USB CDC serial.
// Compatible with the original SDRshield2_0 protocol.
//
// Commands (terminated by CR/LF):
//   f <freq_hz>    — Set VFO to exact frequency in Hz
//   F <freq_khz>   — Set VFO in kHz (×1000 for Hz)
//   A 0 / A 1      — Disable/enable quadrature LO
//   ?              — Query current frequency
//   G <n>          — Set I2S digital gain (1..65536, default 1)
//
// Note: The 'S' (stream) command is removed — audio streaming is now
// controlled by the host selecting the USB Audio interface.
// ============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Callback type for frequency change — return true on success
typedef bool (*freq_change_cb_t)(uint32_t new_freq_hz);

/// Callback type for output enable/disable
typedef void (*output_enable_cb_t)(char output, bool enabled);

/// Callback type for tone generator command
typedef void (*tone_cmd_cb_t)(uint32_t freq_hz);

/// Callback type for debug level command (D 0 / D 1 / D 2)
typedef void (*debug_cmd_cb_t)(int level);

/// Callback type for I2S digital gain command (G <n>)
typedef void (*gain_cmd_cb_t)(uint32_t gain);

/// Initialize the serial command parser.
void serial_cmd_init(freq_change_cb_t on_freq_change,
                     output_enable_cb_t on_output_enable);

/// Register a callback for the T (tone generator) command.
void serial_cmd_set_tone_callback(tone_cmd_cb_t on_tone);

/// Register a callback for the D (debug level) command.
void serial_cmd_set_debug_callback(debug_cmd_cb_t on_debug);

/// Register a callback for the G (I2S gain) command.
void serial_cmd_set_gain_callback(gain_cmd_cb_t on_gain);

/// Set the current frequency (so ? returns the correct value).
void serial_cmd_set_freq(uint32_t freq_hz);

/// Process incoming CDC serial data. Call from main loop.
void serial_cmd_process(void);

/// Send a frequency report back to the host.
void serial_cmd_report_freq(uint32_t freq_hz);

/// Write a raw string to CDC (safe to call even when CDC is disconnected).
void cdc_write_str(const char *str);

/// Write a formatted string to CDC (safe to call even when CDC is disconnected).
void cdc_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
