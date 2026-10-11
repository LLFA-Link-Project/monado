// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to Libsurvive adapter.
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup drv_survive
 */

#pragma once


#include "xrt/xrt_results.h"
#include "xrt/xrt_defines.h"
#include "vive/vive_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_survive Lighthouse tracking using libsurvive
 * @ingroup drv
 *
 * @brief
 */

int
survive_get_devices(struct xrt_prober *xp, struct xrt_device **out_xdevs, struct vive_config **out_vive_config);

// LLFA-only: expose tracking devices, never an HMD. Output capacity is XRT_MAX_DEVICES_PER_PROBE.
// A matching serial permits a formerly HMD-configured tracker to use the existing tracker pose path.
int
survive_get_tracker_devices(struct xrt_prober *xp, struct xrt_device **out_xdevs, const char *selected_serial);

#ifdef __cplusplus
}
#endif
