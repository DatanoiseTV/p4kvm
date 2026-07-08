# Build-time optional: when P4KVM_WG_ENABLE is off the component registers with
# no sources (a header-only INTERFACE lib), so nothing here is compiled. This
# is how the tunnel is made opt-out without deleting the component - targets
# that cannot compile it (e.g. some ESP-IDF 5.5 setups) just turn the option
# off. SRCS gating on CONFIG_ is reliable (resolved in the full build pass);
# gating the dependent's REQUIRES on CONFIG_ is not, because requirements are
# resolved earlier, before sdkconfig is available.
if(CONFIG_P4KVM_WG_ENABLE)
    set(wg_srcs
        "src/crypto.c"
        "src/wireguard.c"
        "src/wireguardif.c"
        "src/wireguard-platform.c"
        "src/crypto/refc/blake2s.c"
        "src/crypto/refc/chacha20.c"
        "src/crypto/refc/chacha20poly1305.c"
        "src/crypto/refc/poly1305-donna.c"
        "src/crypto/refc/x25519.c"
        "src/esp_wireguard.c"
        "src/nacl/crypto_scalarmult/curve25519/ref/smult.c")
else()
    set(wg_srcs "")
endif()

idf_component_register(
    SRCS ${wg_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_netif lwip mbedtls)

if(CONFIG_P4KVM_WG_ENABLE)
    # GCC 15 (IDF 6 toolchain): stringop-overread is a false positive on the
    # variable-limb mul() calls; the WireGuard protocol constants are
    # deliberately unterminated byte arrays.
    set_source_files_properties(src/crypto/refc/x25519.c
        PROPERTIES COMPILE_FLAGS -Wno-error=stringop-overread)
    set_source_files_properties(src/wireguard.c
        PROPERTIES COMPILE_FLAGS -Wno-error=unterminated-string-initialization)
endif()
