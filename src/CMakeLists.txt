idf_component_register(
    SRCS
        "main.c"
        "i2s_audio.c"
        "si5351_quad.c"
        "usb_audio.c"
        "usb_descriptors.c"
        "serial_cmd.c"

    INCLUDE_DIRS
        "../include"
    REQUIRES
        driver
        esp_hw_support
        esp_tinyusb
)

# Audio class defines for our own sources
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CFG_TUD_AUDIO=1
    CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX=2
    CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX=2
    CFG_TUD_AUDIO_FUNC_1_RESOLUTION_TX=16
    CFG_TUD_AUDIO_ENABLE_EP_IN=1
    CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX=196
    "CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ=(48*2*2*16)"
    CFG_TUD_AUDIO_ENABLE_EP_OUT=0
    CFG_TUD_AUDIO_ENABLE_ENCODING=0
    CFG_TUD_AUDIO_ENABLE_DECODING=0
    CFG_TUD_AUDIO_ENABLE_TYPE_I_ENCODING=0
    CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=0
    CFG_TUD_AUDIO_FUNC_1_DESC_LEN=118
    CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ=64
)
