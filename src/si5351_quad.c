// ============================================================================
// Si5351 Quadrature Driver — ESP-IDF I2C Implementation
//
// Direct register-level Si5351 control using ESP-IDF I2C master driver (legacy).
// Replaces the Arduino Etherkit Si5351 library.
//
// Implements only what's needed for quadrature LO generation:
//   - PLL configuration (PLLB) with fractional feedback divider
//   - Multisynth divider configuration (MS0, MS1) with integer mode
//   - Phase offset on CLK0 for 90° quadrature
//   - Output enable/disable
//
// Register reference: Si5351A/B/C datasheet (Skyworks/Silicon Labs)
// ============================================================================

#include "si5351_quad.h"
#include "pins.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "si5351";

// --- I2C port ---
#define SI5351_I2C_PORT   I2C_NUM_0
#define SI5351_I2C_ADDR_7  0x60

// I2C timeout in milliseconds (converted to ticks via pdMS_TO_TICKS)
#define I2C_TIMEOUT_MS  10

// --- State ---
static uint32_t current_lo_freq = 0;
static bool     s_is_variant_a = false;  // Si5351A: SYS_INIT at bit 7, LOL_B at bit 6
static uint8_t  s_lolb_mask = 0x80;      // LOL_B bit: 0x40 for A, 0x80 for B/C

// --- Si5351 Register Addresses ---
#define SI5351_REG_DEVICE_STATUS    0
#define SI5351_REG_OUTPUT_ENABLE    3
#define SI5351_REG_CLK0_CTRL       16
#define SI5351_REG_CLK1_CTRL       17
#define SI5351_REG_CLK2_CTRL       18
#define SI5351_REG_CLK3_0_DIS_STATE 24
#define SI5351_REG_PLLB_BASE       34    // Registers 34–41 for PLLB
#define SI5351_REG_MS0_BASE        42    // Registers 42–49 for Multisynth 0
#define SI5351_REG_MS1_BASE        50    // Registers 50–57 for Multisynth 1
#define SI5351_REG_CLK0_PHASE      165
#define SI5351_REG_CLK1_PHASE      166
#define SI5351_REG_PLL_RESET       177
#define SI5351_REG_PLL_INPUT_SRC   15
#define SI5351_REG_XTAL_CL         183
#define SI5351_PLL_SRC_XO          0x00
#define SI5351_PLL_SRC_PLLB_XO     (0 << 3)
#define SI5351_PLL_SRC_PLLA_XO     (0 << 2)

// --- Crystal parameters ---
#define SI5351_XTAL_FREQ        25000000ULL   // 25 MHz crystal
// Correction in 0.01 Hz units (from calibration)
#define SI5351_CORRECTION       SI5351_XTAL_CORRECTION

// ============================================================================
// I2C Helpers (legacy driver API)
// ============================================================================

static esp_err_t si5351_write_reg(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SI5351_I2C_ADDR_7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(SI5351_I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t si5351_read_reg(uint8_t reg, uint8_t *val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SI5351_I2C_ADDR_7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SI5351_I2C_ADDR_7 << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(SI5351_I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t si5351_write_bulk(uint8_t start_reg, const uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SI5351_I2C_ADDR_7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, start_reg, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(SI5351_I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ============================================================================
// Si5351 PLL / Multisynth Configuration
//
// PLL frequency:   f_VCO = f_XTAL * (a + b/c)
// Output frequency: f_out = f_VCO / (d + e/f)    (for integer mode, e=0, f=1)
//
// For quadrature, d must be an even integer.
// ============================================================================

// Write PLL (feedback) multisynth parameters
// PLL Feedback divider: a + b/c where a is integer part
static esp_err_t si5351_set_pll(uint32_t a, uint32_t b, uint32_t c, bool is_pllb) {
    // Calculate register values per Si5351 datasheet
    uint32_t p1, p2, p3;

    p3 = c;
    p1 = 128 * a + ((128 * b) / c) - 512;
    p2 = 128 * b - c * ((128 * b) / c);

    uint8_t reg_base = is_pllb ? SI5351_REG_PLLB_BASE : 26;  // PLLA=26, PLLB=34
    uint8_t regs[8];

    regs[0] = (uint8_t)((p3 >> 8) & 0xFF);
    regs[1] = (uint8_t)(p3 & 0xFF);
    regs[2] = (uint8_t)((p1 >> 16) & 0x03);
    regs[3] = (uint8_t)((p1 >> 8) & 0xFF);
    regs[4] = (uint8_t)(p1 & 0xFF);
    regs[5] = (uint8_t)(((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F));
    regs[6] = (uint8_t)((p2 >> 8) & 0xFF);
    regs[7] = (uint8_t)(p2 & 0xFF);

    return si5351_write_bulk(reg_base, regs, 8);
}

// Write Multisynth output divider parameters
static esp_err_t si5351_set_ms(uint8_t ms_num, uint32_t divider) {
    // Integer mode: b=0, c=1, so p1 = 128*divider - 512, p2=0, p3=1
    uint32_t p1 = 128 * divider - 512;
    uint32_t p2 = 0;
    uint32_t p3 = 1;

    uint8_t reg_base = SI5351_REG_MS0_BASE + (ms_num * 8);
    uint8_t regs[8];

    regs[0] = (uint8_t)((p3 >> 8) & 0xFF);
    regs[1] = (uint8_t)(p3 & 0xFF);
    regs[2] = (uint8_t)((p1 >> 16) & 0x03);  // R divider = 0, divby4 = 0
    regs[3] = (uint8_t)((p1 >> 8) & 0xFF);
    regs[4] = (uint8_t)(p1 & 0xFF);
    regs[5] = (uint8_t)(((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F));
    regs[6] = (uint8_t)((p2 >> 8) & 0xFF);
    regs[7] = (uint8_t)(p2 & 0xFF);

    return si5351_write_bulk(reg_base, regs, 8);
}
// Calculate even-integer multisynth divider for given output frequency.
// VCO must stay within [600, 900] MHz per Si5351 datasheet.
// Maximises VCO to keep divider small (needed for accurate phase offset).
static uint32_t calc_even_divider(uint32_t output_freq_hz) {
    if (output_freq_hz == 0) return 0;

    uint32_t ideal_div = 900000000UL / output_freq_hz;
    if (ideal_div & 1) ideal_div++;
    if (ideal_div < 6) ideal_div = 6;
    if (ideal_div > 1800) ideal_div = 1800;

    // Walk down if VCO exceeds 900 MHz
    while (1) {
        uint64_t vco = (uint64_t)output_freq_hz * ideal_div;
        if (vco <= 900000000ULL) break;
        if (ideal_div <= 6) break;
        ideal_div -= 2;
    }
    // Walk up if VCO is below 600 MHz
    while (1) {
        uint64_t vco = (uint64_t)output_freq_hz * ideal_div;
        if (vco >= 600000000ULL) break;
        if (ideal_div >= 1800) break;
        ideal_div += 2;
    }

    return ideal_div;
}

// ============================================================================
// Public API
// ============================================================================

bool si5351_quad_init(uint32_t lo_freq_hz) {
    // --- Initialize I2C ---
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SI5351_SDA,
        .scl_io_num = PIN_SI5351_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = i2c_param_config(SI5351_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_driver_install(SI5351_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    // --- Check device is present ---
    uint8_t status;
    err = si5351_read_reg(SI5351_REG_DEVICE_STATUS, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Si5351 not found on I2C (addr 0x%02X)", SI5351_I2C_ADDR_7);
        return false;
    }
    ESP_LOGI(TAG, "Si5351 found, status=0x%02X", status);

    // Detect Si5351 variant from REVID (register 0 bits [4:2]).
    // Si5351A: REVID 0/1, LOL_B at bit 6, SYS_INIT at bit 7.
    // Si5351B: REVID 2/3, LOL_B at bit 7, SYS_INIT at bits 1:0.
    // Si5351C: REVID 4/5, same layout as B.
    uint8_t revid = (status >> 2) & 0x07;
    s_is_variant_a = (revid <= 1);
    s_lolb_mask = s_is_variant_a ? 0x40 : 0x80;
    ESP_LOGI(TAG, "Si5351 rev=%u variant=%s LOL_B=bit%u",
             revid, s_is_variant_a ? "A" : "B/C",
             s_is_variant_a ? 6 : 7);

    // --- Initialize Si5351 ---

    // Disable all outputs during configuration
    si5351_write_reg(SI5351_REG_OUTPUT_ENABLE, 0xFF);

    // Set crystal load capacitance to 8pF (bits 7:6 = 01)
    // Preserve lower bits already set by Si5351 (must keep bits 1 and 4 = 1)
    uint8_t xtal_cl;
    if (si5351_read_reg(SI5351_REG_XTAL_CL, &xtal_cl) == ESP_OK) {
        xtal_cl = (xtal_cl & 0x3F) | (1 << 6);
    } else {
        xtal_cl = (uint8_t)((1 << 6) | 0x12);  // 8pF + default lower bits
    }
    si5351_write_reg(SI5351_REG_XTAL_CL, xtal_cl);

    // Ensure both PLLs use XO as reference (reg 15: PLLA_SRC=0, PLLB_SRC=0)
    si5351_write_reg(SI5351_REG_PLL_INPUT_SRC, SI5351_PLL_SRC_XO);

    // Power down CLK2..CLK7 (not used) — write 0x80 (PDN=1) to each ctrl reg
    for (uint8_t clk = 2; clk <= 7; clk++) {
        si5351_write_reg(SI5351_REG_CLK2_CTRL + (clk - 2), 0x80);
    }

    // Configure CLK0: PLLB source, 2mA drive, integer mode, MS source
    // Register 16: [7]=PDN [6]=MS_INT [5]=PLL_SRC [4:3]=IDRV [2]=INV [1:0]=CLK_SRC
    // CLK_PDN=0, MS_INT=1, PLLB=1, IDRV=00(2mA), INV=0, CLK_SRC=11(MSn)
    si5351_write_reg(SI5351_REG_CLK0_CTRL, 0x63);  // 0b01100011

    // Configure CLK1 identically
    si5351_write_reg(SI5351_REG_CLK1_CTRL, 0x63);  // 0b01100011

    // Set initial LO frequency with quadrature
    if (!si5351_quad_set_freq(lo_freq_hz)) {
        return false;
    }

    // Enable CLK0 and CLK1 outputs
    si5351_quad_enable(true);

    ESP_LOGI(TAG, "Si5351 initialized: LO = %lu Hz, quadrature on CLK0/CLK1", lo_freq_hz);
    return true;
}

bool si5351_quad_set_freq(uint32_t lo_freq_hz) {
    if (lo_freq_hz < MIN_LO_FREQ_HZ || lo_freq_hz > MAX_LO_FREQ_HZ) {
        ESP_LOGE(TAG, "Frequency %lu Hz out of range", lo_freq_hz);
        return false;
    }

    // Calculate even-integer multisynth divider
    uint32_t divider = calc_even_divider(lo_freq_hz);

    // VCO frequency = output_freq * divider
    uint64_t vco_freq = (uint64_t)lo_freq_hz * divider;

    // Apply crystal correction
    // Corrected crystal frequency in 0.01 Hz = 25000000 * 100 + correction
    uint64_t xtal_freq_100 = (uint64_t)SI5351_XTAL_FREQ * 100ULL + SI5351_CORRECTION;

    // PLL feedback divider = VCO / XTAL
    // a + b/c = vco_freq / xtal_freq
    // Using fixed denominator c = 1000000 for precision
    uint32_t c_denom = 1000000;
    uint64_t vco_100 = vco_freq * 100ULL;  // in 0.01 Hz units

    uint32_t a = (uint32_t)(vco_100 / xtal_freq_100);
    uint64_t remainder = vco_100 - (uint64_t)a * xtal_freq_100;
    uint32_t b = (uint32_t)((remainder * c_denom) / xtal_freq_100);

    ESP_LOGI(TAG, "freq=%lu div=%lu VCO=%llu a=%lu b=%lu c=%lu",
             lo_freq_hz, divider, vco_freq, a, b, c_denom);

    if (vco_freq < 600000000ULL || vco_freq > 900000000ULL) {
        ESP_LOGW(TAG, "VCO %llu Hz out of Si5351 spec [600–900 MHz]; PLL may be unstable", vco_freq);
    }

    // Program PLLB with feedback divider a + b/c
    esp_err_t wr_err = si5351_set_pll(a, b, c_denom, true);
    if (wr_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write PLLB params failed: %s", esp_err_to_name(wr_err));
        return false;
    }

    // Program MS0 and MS1 with the same integer divider
    wr_err = si5351_set_ms(0, divider);
    if (wr_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write MS0 params failed: %s", esp_err_to_name(wr_err));
        return false;
    }
    wr_err = si5351_set_ms(1, divider);
    if (wr_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write MS1 params failed: %s", esp_err_to_name(wr_err));
        return false;
    }

    // Set CLK1 phase offset = divider (gives 90° at output, CLK0 leads).
    // Phase register is 7 bits; warn if divider exceeds 127 (quadrature degraded).
    uint8_t phase_val;
    if (divider <= 127) {
        phase_val = (uint8_t)divider;
    } else {
        phase_val = 127;
        float actual_deg = ((float)phase_val * 90.0f) / (float)divider;
        ESP_LOGW(TAG, "Divider %lu > 127, quadrature degraded: "
                 "phase=%.1f° (ideal 90°, freq=%lu Hz)",
                 divider, actual_deg, lo_freq_hz);
    }
    si5351_write_reg(SI5351_REG_CLK0_PHASE, 0);
    si5351_write_reg(SI5351_REG_CLK1_PHASE, phase_val);

    // Reset PLLB to apply new frequency and phase relationship.
    // Per Si5351 AN619: assert the reset bit, then release it.
    // The hardware does NOT auto-clear — the host must write 0 to release.
    wr_err = si5351_write_reg(SI5351_REG_PLL_RESET, 0x80);
    if (wr_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write PLL reset assert failed: %s", esp_err_to_name(wr_err));
        return false;
    }
    wr_err = si5351_write_reg(SI5351_REG_PLL_RESET, 0x00);
    if (wr_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write PLL reset release failed: %s", esp_err_to_name(wr_err));
        return false;
    }

    // Wait for PLL calibration to complete, then poll lock.
    // Si5351A: SYS_INIT flag clears when calibration finishes.
    // Si5351B/C: SYS_INIT bits 1:0 auto-clear faster; just delay briefly.
    // After calibration, poll register 0 (live status) for LOL_B.
    int lock_retries = 50;
    bool pll_locked = false;
    bool i2c_err_logged = false;
    while (lock_retries-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
        uint8_t status;
        esp_err_t e = si5351_read_reg(SI5351_REG_DEVICE_STATUS, &status);
        if (e != ESP_OK) {
            if (!i2c_err_logged) {
                ESP_LOGE(TAG, "I2C read failed during PLL lock poll: %s", esp_err_to_name(e));
                i2c_err_logged = true;
            }
            continue;
        }
        // Wait for SYS_INIT to clear and LOL_B to clear (LOL=0 means locked)
        if (s_is_variant_a) {
            if (status & 0x80) continue;  // SYS_INIT still high, wait
        } else {
            if (status & 0x03) continue;  // SYS_INIT[1:0] still active, wait
        }
        if (!(status & s_lolb_mask)) {
            pll_locked = true;
            break;
        }
    }
    if (pll_locked) {
        current_lo_freq = lo_freq_hz;
        ESP_LOGI(TAG, "PLLB locked: freq=%lu Hz div=%lu VCO=%llu a=%lu b=%lu c=%lu",
                 lo_freq_hz, divider, vco_freq, a, b, c_denom);

        // Read-back verify: confirm PLLB and MS0 registers were written
        uint8_t verify[8];
        if (si5351_read_reg(SI5351_REG_PLLB_BASE, &verify[0]) == ESP_OK) {
            uint8_t expected_p3_hi = (uint8_t)((c_denom >> 8) & 0xFF);
            if (verify[0] != expected_p3_hi) {
                ESP_LOGW(TAG, "PLLB reg readback mismatch: wrote 0x%02X, read 0x%02X",
                         expected_p3_hi, verify[0]);
            } else {
                ESP_LOGI(TAG, "PLLB readback OK (reg34=0x%02X)", verify[0]);
            }
        }
        if (si5351_read_reg(SI5351_REG_MS0_BASE + 3, &verify[3]) == ESP_OK) {
            uint8_t expected_p1_mid = (uint8_t)(((128 * divider - 512) >> 8) & 0xFF);
            if (verify[3] != expected_p1_mid) {
                ESP_LOGW(TAG, "MS0 reg readback mismatch: wrote 0x%02X, read 0x%02X",
                         expected_p1_mid, verify[3]);
            } else {
                ESP_LOGI(TAG, "MS0 readback OK (reg45=0x%02X)", verify[3]);
            }
        }
    } else {
        ESP_LOGW(TAG, "PLLB did not lock within ~55ms — registers programmed, PLL may lock shortly (freq=%lu Hz div=%lu VCO=%llu)",
                 lo_freq_hz, divider, vco_freq);
    }

    return pll_locked;
}

uint32_t si5351_quad_get_freq(void) {
    return current_lo_freq;
}

bool si5351_quad_enable(bool enabled) {
    uint8_t val;
    esp_err_t err = si5351_read_reg(SI5351_REG_OUTPUT_ENABLE, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read OEB register, skipping enable update");
        return false;
    }
    if (enabled) {
        val &= ~0x03;  // Enable CLK0, CLK1 (active low)
    } else {
        val |= 0x03;   // Disable CLK0, CLK1
    }
    si5351_write_reg(SI5351_REG_OUTPUT_ENABLE, val);
    return true;
}
