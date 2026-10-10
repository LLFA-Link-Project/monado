// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_prober.h"
#ifdef __cplusplus
extern "C" {
#endif
struct os_hid_device;
/* Takes ownership of hid, including on failure. */
struct xrt_device *
arpara_create(struct os_hid_device *hid);
int
arpara_found(struct xrt_prober *xp,
             struct xrt_prober_device **devices,
             size_t device_count,
             size_t index,
             cJSON *attached_data,
             struct xrt_device **out_xdevs);
#ifdef __cplusplus
}
#endif
