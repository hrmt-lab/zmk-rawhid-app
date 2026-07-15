/*
 * Copyright (c) 2026 Keylink Studio Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <tinycrypt/sha256.h>

#include <rawhid_app/behavior_identity.h>

static void hash_update_u8(struct tc_sha256_state_struct *sha, uint8_t value) {
    tc_sha256_update(sha, &value, sizeof(value));
}

static void hash_update_string(struct tc_sha256_state_struct *sha, const char *value) {
    uint16_t len = value == NULL ? 0 : (uint16_t)strlen(value);
    uint8_t len_bytes[2];
    sys_put_le16(len, len_bytes);
    tc_sha256_update(sha, len_bytes, sizeof(len_bytes));
    if (len > 0) {
        tc_sha256_update(sha, (const uint8_t *)value, len);
    }
}

void rawhid_app_behavior_identity_hash(const struct zmk_behavior_binding *binding,
                                       uint8_t *hash, size_t hash_len) {
    struct tc_sha256_state_struct sha;
    uint8_t digest[TC_SHA256_DIGEST_SIZE];

    if (hash == NULL || hash_len == 0) {
        return;
    }

    tc_sha256_init(&sha);
    hash_update_u8(&sha, RAWHID_APP_BEHAVIOR_IDENTITY_SCHEMA_VERSION);
    hash_update_string(&sha, binding == NULL ? NULL : binding->behavior_dev);
    /* v1 has two binding cells and no stable behavior-compatible string. */
    hash_update_u8(&sha, 2);
    hash_update_string(&sha, "");
    hash_update_string(&sha, "");
    tc_sha256_final(digest, &sha);
    memcpy(hash, digest, MIN(hash_len, sizeof(digest)));
}
