set(p4kvm_capture_srcs
    "atx_ctrl.c"
    "audio_stream.c"
    "capture.c"
    "capture_hw.c"
    "capture_h264.c"
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
    "usb_serial.c"
    "video_mode.c"
    "webrtc_kvm.c"
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
        bootloader_support
        espressif__cjson
        espressif__esp_tinyusb
        espressif__esp_h264
        espressif__esp_peer
        # esp_wireguard stays in REQUIRES unconditionally so main always sees
        # its header; the component itself compiles to nothing when
        # P4KVM_WG_ENABLE is off (see components/esp_wireguard/CMakeLists.txt),
        # which is the reliable way to make it build-time optional - REQUIRES
        # are resolved before sdkconfig in the early-expansion phase, so gating
        # them on CONFIG_ does not work.
        esp_wireguard
)

# esp_h264 1.3.6 on IDF 6.0.0: force-include the compatibility shim into every
# esp_h264 translation unit so its use of esp_efuse_is_flash_encryption_enabled()
# (which IDF 6.0.0 does not ship) resolves to a declaration; main/h264_compat.c
# provides the definition. Applied unconditionally because esp_h264 is compiled
# even when WebRTC is disabled. See main/h264_efuse_compat.h.
idf_component_get_property(h264_lib espressif__esp_h264 COMPONENT_LIB)
target_compile_options(${h264_lib} PRIVATE
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/h264_efuse_compat.h")

# The BitScrambler reorder pass turned out to be unnecessary (the JPEG
# encoder consumes native UYVY; see capture_priv.h) and measured ~28 MB/s -
# too slow for 1080p30 anyway. uyvy_to_yvyu.bsasm stays in the tree for
# reference; if CAPTURE_NEEDS_REORDER is ever set again, re-add:
#   target_bitscrambler_add_src("uyvy_to_yvyu.bsasm")
