// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"
#ifdef __cplusplus
extern "C" {
#endif

// Parse metres and quaternion x,y,z,w. Reject non-finite values and normalize rotation.
bool
t_llfa_parse_mount(const char *text, struct xrt_pose *mount);

// -1: absent, -2: ambiguous/unsupported. An explicit serial never falls back to another device.
int
t_llfa_select_tracker(struct xrt_device **devices, uint32_t count, const char *serial);

// Replace the arpara slot with a DIRECT wrapper. System owns tracker separately; wrapper owns arpara.
xrt_result_t
t_llfa_wrap_head(struct xrt_system_devices *system,
                 uint32_t head_index,
                 uint32_t tracker_index,
                 const struct xrt_pose *mount);

#ifdef __cplusplus
}
#endif
