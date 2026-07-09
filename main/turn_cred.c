/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "turn_cred.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

/* Sanity floor: any real SNTP-synced time is far past this (2021-01-01). If the
 * clock reads below it, time has not synced and a derived username (an expiry
 * timestamp) would be meaningless. */
#define TURN_TIME_SYNCED_FLOOR 1609459200

esp_err_t turn_cred_make(const char *secret, int ttl_s,
                         char *user, size_t user_sz,
                         char *pass, size_t pass_sz)
{
    if (!secret || !secret[0] || !user || !pass || ttl_s <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    time_t now = time(NULL);
    if (now < TURN_TIME_SYNCED_FLOOR) {
        return ESP_ERR_INVALID_STATE; /* clock not set yet */
    }

    /* username = expiry unix timestamp (coturn validates now < expiry). */
    int n = snprintf(user, user_sz, "%lld", (long long)(now + ttl_s));
    if (n <= 0 || (size_t)n >= user_sz) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* password = base64(HMAC-SHA1(secret, username)). */
    unsigned char mac[20];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) {
        return ESP_FAIL;
    }
    if (mbedtls_md_hmac(info, (const unsigned char *)secret, strlen(secret),
                        (const unsigned char *)user, (size_t)n, mac) != 0) {
        return ESP_FAIL;
    }

    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)pass, pass_sz, &olen, mac, sizeof(mac)) != 0) {
        return ESP_ERR_INVALID_SIZE; /* pass buffer too small (needs >= 29) */
    }
    return ESP_OK;
}
