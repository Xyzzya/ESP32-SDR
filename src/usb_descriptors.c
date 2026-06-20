// ============================================================================
// USB Descriptors — Composite CDC + UAC2
//
// Descriptor layout:
//   Interface 0: CDC Communication (ACM)
//   Interface 1: CDC Data
//   Interface 2: Audio Control (AC)
//   Interface 3: Audio Streaming (AS) — Microphone IN (I/Q audio to host)
//
// The device appears to the host as:
//   - A serial port (COM port / /dev/ttyACMx)
//   - A USB microphone named "ESP32 SDR Receiver"
// ============================================================================

#include "tusb.h"
#include "class/audio/audio.h"

// --- String Descriptor Indices ---
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_AUDIO,
};

// --- Interface Numbers ---
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_AUDIO_CONTROL,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_TOTAL,
};

// --- Endpoint Numbers ---
#define EPNUM_CDC_NOTIF     0x81
#define EPNUM_CDC_OUT       0x02
#define EPNUM_CDC_IN        0x82
#define EPNUM_AUDIO_IN      0x83

// EP max packet size for audio (48 frames × 2ch × 2 bytes = 192, plus margin)
#define AUDIO_EP_MAX_PKT    196

// --- Device Descriptor ---
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,       // Espressif VID
    .idProduct          = 0x8001,       // Custom PID for SDR
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

// --- Configuration Descriptor ---

// Interface Association Descriptor (IAD) for the Audio function
#define TUD_AUDIO_IAD_LEN 8

// Class-specific Audio Control: AC Header(9) + Clock Source(8) + Input Terminal(17) + Output Terminal(12)
#define TUD_AUDIO_AC_CS_LEN  (9 + 8 + 17 + 12)

// Total Audio Control Interface length (Standard AC Interface + Class-specific AC descriptors)
#define TUD_AUDIO_AC_TOTAL_LEN (9 + TUD_AUDIO_AC_CS_LEN)

// Audio Streaming: IF alt0(9) + IF alt1(9) + AS Header(16) + Format(6) + EP(7) + EP CS(8)
#define TUD_AUDIO_AS_LEN  (9 + 9 + 16 + 6 + 7 + 8)

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_AUDIO_IAD_LEN + TUD_AUDIO_AC_TOTAL_LEN + TUD_AUDIO_AS_LEN)

uint8_t const desc_configuration[] = {
    // ---- Configuration Descriptor ----
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 250),

    // ---- CDC ----
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    // ================================================================
    // UAC2 Audio Function (IAD + AC + AS)
    // ================================================================
    
    // Interface Association Descriptor
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    ITF_NUM_AUDIO_CONTROL, 2,
    TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_UNDEFINED, AUDIO_FUNC_PROTOCOL_CODE_V2,
    0,

    // ================================================================
    // UAC2 Audio Control Interface (interface 2)
    // ================================================================

    // Standard AC Interface Descriptor
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_CONTROL, 0, 0,
    TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL,
    AUDIO_FUNC_PROTOCOL_CODE_V2,
    STRID_AUDIO,

    // Class-Specific AC Header (UAC2)
    9, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_HEADER,
    U16_TO_U8S_LE(0x0200),             // bcdADC: 2.0
    AUDIO_FUNC_MICROPHONE,              // bCategory
    U16_TO_U8S_LE(TUD_AUDIO_AC_CS_LEN),// wTotalLength
    0x00,                               // bmControls

    // Clock Source (ID=1) — internal fixed clock
    8, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_CLOCK_SOURCE,
    1,                                  // bClockID
    AUDIO_CLOCK_SOURCE_ATT_INT_FIX_CLK, // bmAttributes: internal fixed
    (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_FRQ_POS) |
    (AUDIO_CTRL_R << AUDIO_CLOCK_SOURCE_CTRL_CLK_VAL_POS), // bmControls: freq readable, validity readable
    0,                                  // bAssocTerminal
    0,                                  // iClockSource

    // Input Terminal (ID=2) — Microphone
    17, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_INPUT_TERMINAL,
    2,                                  // bTerminalID
    U16_TO_U8S_LE(AUDIO_TERM_TYPE_IN_GENERIC_MIC),
    0,                                  // bAssocTerminal
    1,                                  // bCSourceID: Clock Source #1
    2,                                  // bNrChannels
    U32_TO_U8S_LE(0x00000003),          // bmChannelConfig: Front Left, Front Right
    0,                                  // iChannelNames
    U16_TO_U8S_LE(0x0000),             // bmControls
    0,                                  // iTerminal

    // Output Terminal (ID=3) — USB Streaming (to Host)
    12, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_OUTPUT_TERMINAL,
    3,                                  // bTerminalID
    U16_TO_U8S_LE(AUDIO_TERM_TYPE_USB_STREAMING),
    0,                                  // bAssocTerminal
    2,                                  // bSourceID: Input Terminal #2
    1,                                  // bCSourceID: Clock Source #1
    U16_TO_U8S_LE(0x0000),              // bmControls
    0,                                  // iTerminal

    // ================================================================
    // UAC2 Audio Streaming Interface (interface 3)
    // ================================================================

    // AS Interface Alt 0 (zero-bandwidth)
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_STREAMING, 0, 0,
    TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING,
    AUDIO_FUNC_PROTOCOL_CODE_V2, 0,

    // AS Interface Alt 1 (active streaming)
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_STREAMING, 1, 1,
    TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING,
    AUDIO_FUNC_PROTOCOL_CODE_V2, 0,

    // Class-Specific AS General
    16, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AS_INTERFACE_AS_GENERAL,
    3,                                  // bTerminalLink: Output Terminal #3
    0x00,                               // bmControls
    AUDIO_FORMAT_TYPE_I,
    U32_TO_U8S_LE(AUDIO_DATA_FORMAT_TYPE_I_PCM),
    2,                                  // bNrChannels
    U32_TO_U8S_LE(0x00000003),          // bmChannelConfig: Front Left, Front Right
    0,                                  // iChannelNames

    // Format Type I
    6, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AS_INTERFACE_FORMAT_TYPE,
    AUDIO_FORMAT_TYPE_I,
    2,                                  // bSubSlotSize: 2 bytes
    16,                                 // bBitResolution: 16 bits

    // Standard AS Isochronous Audio Data Endpoint (IN)
    7, TUSB_DESC_ENDPOINT,
    EPNUM_AUDIO_IN,
    (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA), // Isochronous, Adaptive, Data
    U16_TO_U8S_LE(AUDIO_EP_MAX_PKT),
    1,                                  // bInterval

    // Class-Specific AS Isochronous Audio Data Endpoint
    8, TUSB_DESC_CS_ENDPOINT, AUDIO_CS_EP_SUBTYPE_GENERAL,
    0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0000),
};

// --- String Descriptors ---
char const* string_desc_arr[] = {
    (const char[]){0x09, 0x04},     // [0] Language: English (US), raw LANGID only (header auto-generated by esp_tinyusb)
    "ESP32-S3 SDR",                 // [1] Manufacturer
    "ESP32 SDR Receiver",           // [2] Product
    "SDR-001",                      // [3] Serial
    "SDR Serial Port",              // [4] CDC
    "SDR I/Q Audio",                // [5] Audio
};
_Static_assert(sizeof(string_desc_arr) / sizeof(string_desc_arr[0]) == 6,
               "string_desc_arr[] count changed — update string_descriptor_count in main.c");


