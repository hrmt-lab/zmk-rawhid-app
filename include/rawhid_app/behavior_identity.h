/*
 * Copyright (c) 2026 Keylink Studio Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define RAWHID_APP_BEHAVIOR_IDENTITY_SCHEMA_VERSION 1

/* Produces the prefix of the shared v1 behavior-identity SHA-256 digest. */
void rawhid_app_behavior_identity_hash(const struct zmk_behavior_binding *binding,
                                       uint8_t *hash, size_t hash_len);
