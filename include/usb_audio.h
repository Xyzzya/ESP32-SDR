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
// Coexists with USB CDC serial on the same composite USB device.
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize USB audio (UAC2 microphone mode).
/// @return
bool usb_audio_init(void);


/// @param samples   Interleaved 16-bit stereo samples [L0, R0, L1, R1, ...]
/// @param frames
/// @return
bool usb_audio_write(const int16_t *samples, size_t frames);

bool usb_audio_is_streaming(void);

#ifdef __cplusplus
}
#endif
