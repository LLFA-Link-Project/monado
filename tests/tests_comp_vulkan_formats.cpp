// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "util/comp_vulkan.h"

TEST_CASE("All supported Vulkan formats fit without corrupting compositor info", "[vulkan][formats]")
{
	comp_vulkan_formats supported = {};
#define ENABLE(F) supported.has_##F = true;
	VK_CSCI_FORMATS(ENABLE, ENABLE, ENABLE, ENABLE)
#undef ENABLE
	struct
	{
		xrt_compositor_info info;
		uint64_t canary;
	} result = {};
	result.info.max_texture_size = 4096;
	result.canary = UINT64_C(0xdeadbeef12345678);
	comp_vulkan_formats_copy_to_info(&supported, &result.info);
	REQUIRE(result.info.format_count == 19);
	REQUIRE(result.info.format_count <= XRT_MAX_SWAPCHAIN_FORMATS);
	REQUIRE(result.info.formats[0] == VK_FORMAT_R16G16B16A16_UNORM);
	REQUIRE(result.info.formats[18] == VK_FORMAT_S8_UINT);
	REQUIRE(result.info.max_texture_size == 4096);
	REQUIRE(result.canary == UINT64_C(0xdeadbeef12345678));

	supported = {};
	comp_vulkan_formats_copy_to_info(&supported, &result.info);
	REQUIRE(result.info.format_count == 0);
	supported.has_B8G8R8A8_UNORM = true;
	comp_vulkan_formats_copy_to_info(&supported, &result.info);
	REQUIRE(result.info.format_count == 1);
	REQUIRE(result.info.formats[0] == VK_FORMAT_B8G8R8A8_UNORM);
}
