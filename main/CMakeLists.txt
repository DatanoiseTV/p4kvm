set(p4kvm_capture_srcs
    "atx_ctrl.c"
    "capture.c"
    "capture_hw.c"
    "capture_mjpeg.c"
    "capture_testpat.c"
    "http_server.c"
    "ethernet.c"
    "net_common.c"
    "wifi_net.c"
    "jpeg_frame.c"
    "main.c"
    "tc358743.c"
    "usb_hid.c"
)

idf_component_register(
    SRCS ${p4kvm_capture_srcs}
    INCLUDE_DIRS "."
    EMBED_FILES
        "index.html"
        "favicon.ico"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_cam
        esp_driver_isp
        esp_http_server
        esp_eth
        esp_netif
        esp_wifi
        mdns
        esp_event
        nvs_flash
        esp_hw_support
        esp_psram
        esp_driver_jpeg
        esp_driver_bitscrambler
        esp_mm
        esp_timer
        mbedtls
        espressif__esp_tinyusb
)

if(CONFIG_P4KVM_PIPELINE_YUV422_BS)
    # Assembles uyvy_to_yvyu.bsasm and embeds it as bitscrambler_program_uyvy_to_yvyu.
    target_bitscrambler_add_src("uyvy_to_yvyu.bsasm")
endif()
