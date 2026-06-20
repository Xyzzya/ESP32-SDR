#pragma once
// ============================================================================
// USB Audio Streaming — UAC2 (USB Audio Class 2.0)
//
// The ESP32-S3 appears as a USB microphone ("ESP32 SDR Receiver") that
// streams 48 kHz / 16-bit / stereo I/Q audio directly to the host.
//
// HDSDR, SDR#, or any audio application simply selects this device as
// its audio input — no virtual cables, no Python bridge needed.
//
// Coexists with USB CDC serial on the same composite USB device, so the
// web control interface can tune the Si5351 while audio streams.
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize USB audio (UAC2 microphone mode).
/// @return true on success
bool usb_audio_init(void);

/// Feed audio samples directly to the TinyUSB TX FIFO.
/// @param samples   Interleaved 16-bit stereo samples [L0, R0, L1, R1, ...]
/// @param frames    Number of stereo frames
/// @return true if data was successfully queued
bool usb_audio_write(const int16_t *samples, size_t frames);

/// Service TinyUSB stack — call from main task loop.
void usb_audio_task(void);

/// Check if the host is actively capturing audio.
/// Returns true when the host has selected alt setting 1 on the audio interface.
bool usb_audio_is_streaming(void);

/// Returns true if it's safe to send text on CDC serial.
/// Always true now — CDC and Audio are separate USB interfaces.
bool usb_audio_is_text_ok(void);

#ifdef __cplusplus
}
#endif
