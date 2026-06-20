#pragma once
// ============================================================================
// Si5351 Quadrature Driver (ESP-IDF I2C)
//
// Generates CLK0 and CLK1 with 90° phase offset for I/Q demodulation
// via the 74HC4052 Tayloe detector.
//
// Uses ESP-IDF I2C driver directly — no Arduino Wire dependency.
// ============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize Si5351 on I2C and set up quadrature outputs.
/// @param lo_freq_hz  Desired LO frequency in Hz
/// @return true on success
bool si5351_quad_init(uint32_t lo_freq_hz);

/// Change the LO frequency. Recalculates dividers and resets PLL phase.
/// @param lo_freq_hz  New LO frequency in Hz
/// @return true on success
bool si5351_quad_set_freq(uint32_t lo_freq_hz);

/// Get the current LO frequency in Hz.
uint32_t si5351_quad_get_freq(void);

/// Enable or disable the quadrature outputs (CLK0 + CLK1).
/// @return true on success
bool si5351_quad_enable(bool enabled);

#ifdef __cplusplus
}
#endif
