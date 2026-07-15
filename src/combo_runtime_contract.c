/*
 * Copyright (c) 2026 Keylink Studio Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#if DT_NODE_EXISTS(DT_PATH(combos))
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DT_PATH(combos), disabled) ||
                 IS_ENABLED(CONFIG_RAWHID_APP_COMBO_RUNTIME),
             "disabled /combos requires CONFIG_RAWHID_APP_COMBO_RUNTIME=y");
BUILD_ASSERT(!IS_ENABLED(CONFIG_RAWHID_APP_COMBO_RUNTIME) ||
                 DT_NODE_HAS_STATUS(DT_PATH(combos), disabled),
             "CONFIG_RAWHID_APP_COMBO_RUNTIME=y requires /combos status = \"disabled\"");
#else
BUILD_ASSERT(!IS_ENABLED(CONFIG_RAWHID_APP_COMBO_RUNTIME),
             "CONFIG_RAWHID_APP_COMBO_RUNTIME=y requires a disabled /combos node");
#endif
