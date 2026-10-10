// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_device.h"
#include <string.h>

/* Output profile for the tested DP mode, not a panel or optical calibration.
 * Preserve the driver's provisional, unrotated side-by-side ordering. Do not
 * change display.w_pixels/h_pixels: those describe application image sizes.
 */
static inline bool
comp_drm_arpara_output_viewports(const struct xrt_device *device,
                                 uint32_t width,
                                 uint32_t height,
                                 struct xrt_rect out[XRT_MAX_VIEWS])
{
	if (strcmp(device->str, "arpara 5K") != 0 || strcmp(device->serial, "arpara 5K (USB HID)") != 0 ||
	    !device->hmd || device->hmd->view_count != 2 || width != 3840 || height != 1920) {
		return false;
	}
	const struct xrt_hmd_parts *hmd = device->hmd;
	uint32_t source_width = hmd->screens[0].w_pixels, source_height = hmd->screens[0].h_pixels;
	if (!source_width || !source_height || source_width % 2) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		const struct xrt_view *v = &hmd->views[i];
		if (v->viewport.x_pixels != i * (source_width / 2) || v->viewport.y_pixels != 0 ||
		    v->viewport.w_pixels != source_width / 2 || v->viewport.h_pixels != source_height ||
		    v->rot.v[0] != 1 || v->rot.v[1] != 0 || v->rot.v[2] != 0 || v->rot.v[3] != 1) {
			return false;
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		out[i].offset.w = i * 1920;
		out[i].offset.h = 0;
		out[i].extent.w = 1920;
		out[i].extent.h = 1920;
	}
	return true;
}
