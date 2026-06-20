#pragma once
// ============================================================================
// I2S Audio Capture — PCM1808 ADC Interface
//
// ESP32-S3 as I2S master, PCM1808 in slave mode.
// ESP32-S3 provides: BCK, LRCK, MCLK (via I2S peripheral MCLK output)
// PCM1808 provides: serial data (OUT)
//
// Stereo 48 kHz, 32-bit slots (PCM1808 outputs 24-bit left-justified)
// Left channel = I, Right channel = Q
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize I2S peripheral in master mode with MCLK output.
/// @return true on success
bool i2s_audio_init(void);

/// Read a block of stereo samples from the I2S DMA buffer.
/// @param buffer     Output buffer for raw 32-bit samples [L0, R0, L1, R1, ...]
/// @param num_frames Number of stereo frames to read (each frame = 2 samples)
/// @param timeout_ticks  Maximum ticks to wait for data
/// @return Number of frames actually read
size_t i2s_audio_read(int32_t *buffer, size_t num_frames, uint32_t timeout_ticks);

/// Convert 32-bit I2S samples to 16-bit for USB audio.
/// @param in      Input 32-bit sample buffer
/// @param out     Output 16-bit sample buffer
/// @param frames  Number of stereo frames
void i2s_audio_convert_to_16bit(const int32_t *in, int16_t *out, size_t frames);

/// Reset I2S DMA engine — flushes buffers and restarts the channel.
/// Call on streaming start to ensure clean sample alignment.
void i2s_audio_reset(void);

#ifdef __cplusplus
}
#endif
