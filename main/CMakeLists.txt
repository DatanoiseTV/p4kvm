set(p4kvm_capture_srcs
    "atx_ctrl.c"
    "audio_stream.c"
    "capture.c"
    "capture_hw.c"
    "capture_mjpeg.c"
    "capture_testpat.c"
    "http_server.c"
    "ethernet.c"
    "net_common.c"
    "wifi_net.c"
    "wireguard_net.c"
    "jpeg_frame.c"
    "main.c"
    "runtime_cfg.c"
    "tc358743.c"
    "usb_hid.c"
    "usb_msc.c"
    "video_mode.c"
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
        esp_driver_i2s
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
        espressif__cjson
        espressif__esp_tinyusb
        esp_wireguard
)

# The BitScrambler reorder pass turned out to be unnecessary (the JPEG
# encoder consumes native UYVY; see capture_priv.h) and measured ~28 MB/s -
# too slow for 1080p30 anyway. uyvy_to_yvyu.bsasm stays in the tree for
# reference; if CAPTURE_NEEDS_REORDER is ever set again, re-add:
#   target_bitscrambler_add_src("uyvy_to_yvyu.bsasm")
