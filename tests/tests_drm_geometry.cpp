// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "comp_drm_geometry.h"

TEST_CASE("arpara DRM output profile preserves app geometry and validates SBS layout", "[drm][geometry]")
{
	xrt_device device = {};
	xrt_hmd_parts hmd = {};
	device.hmd = &hmd;
	strcpy(device.str, "arpara 5K");
	strcpy(device.serial, "arpara 5K (USB HID)");
	hmd.view_count = 2;
	hmd.screens[0].w_pixels = 2560;
	hmd.screens[0].h_pixels = 1440;
	for (unsigned i = 0; i < 2; i++) {
		hmd.views[i].viewport = {i * 1280, 0, 1280, 1440};
		hmd.views[i].display.w_pixels = 1280;
		hmd.views[i].display.h_pixels = 1440;
		hmd.views[i].rot.v[0] = hmd.views[i].rot.v[3] = 1;
	}
	xrt_rect viewports[XRT_MAX_VIEWS] = {};
	REQUIRE(comp_drm_arpara_output_viewports(&device, 3840, 1920, viewports));
	REQUIRE(viewports[0].offset.w == 0);
	REQUIRE(viewports[1].offset.w == 1920);
	for (unsigned i = 0; i < 2; i++) {
		REQUIRE(viewports[i].extent.w == 1920);
		REQUIRE(viewports[i].extent.h == 1920);
		REQUIRE(viewports[i].offset.h == 0);
		REQUIRE(hmd.views[i].display.w_pixels == 1280);
		REQUIRE(hmd.views[i].display.h_pixels == 1440);
	}
	REQUIRE(hmd.screens[0].w_pixels == 2560);
	REQUIRE_FALSE(comp_drm_arpara_output_viewports(&device, 1920, 1080, viewports));
	hmd.views[1].viewport.x_pixels = 0;
	REQUIRE_FALSE(comp_drm_arpara_output_viewports(&device, 3840, 1920, viewports));
	hmd.views[1].viewport.x_pixels = 1280;
	hmd.views[1].rot.v[0] = 0;
	REQUIRE_FALSE(comp_drm_arpara_output_viewports(&device, 3840, 1920, viewports));
	hmd.views[1].rot.v[0] = 1;
	strcpy(device.str, "other HMD");
	REQUIRE_FALSE(comp_drm_arpara_output_viewports(&device, 3840, 1920, viewports));
}
