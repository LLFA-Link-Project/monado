// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "vk/vk_helpers.h"

TEST_CASE("Vulkan queue mutex cleanup before device initialization", "[vk][mutex]")
{
	struct vk_bundle vk = {};
	REQUIRE(vk_deinit_mutex(&vk) == VK_SUCCESS);
	REQUIRE(vk.queue_mutex_count == 0);
}

TEST_CASE("Vulkan queue mutexes can be initialized and cleaned up repeatedly", "[vk][mutex]")
{
	struct vk_bundle vk = {};
	for (unsigned iteration = 0; iteration < 2; iteration++) {
		REQUIRE(vk_init_mutex(&vk) == VK_SUCCESS);
		REQUIRE(vk.queue_mutex_count == VK_BUNDLE_MAX_QUEUES);
		for (unsigned i = 0; i < vk.queue_mutex_count; i++) {
			REQUIRE(os_mutex_trylock(&vk.queues[i].mutex) == 0);
			os_mutex_unlock(&vk.queues[i].mutex);
		}
		REQUIRE(vk_deinit_mutex(&vk) == VK_SUCCESS);
		REQUIRE(vk.queue_mutex_count == 0);
		REQUIRE(vk_deinit_mutex(&vk) == VK_SUCCESS);
	}
}
