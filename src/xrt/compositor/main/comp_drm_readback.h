// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
/* CPU readback diagnostic, deliberately independent of WSI and external memory. */
struct comp_drm_readback;
struct comp_drm_readback *
comp_drm_readback_create(VkPhysicalDevice physical,
                         VkDevice device,
                         uint32_t family,
                         uint32_t width,
                         uint32_t height,
                         VkFormat format,
                         VkImageUsageFlags usage);
VkImage
comp_drm_readback_image(struct comp_drm_readback *r);
VkImageView
comp_drm_readback_view(struct comp_drm_readback *r);
/* Queue must be externally locked. Wait semaphore is binary. Optional clear
 * renders a standalone test pattern; otherwise image must be in GENERAL. */
VkResult
comp_drm_readback_copy(
    struct comp_drm_readback *r, VkQueue queue, VkSemaphore wait, bool clear, const uint8_t **pixels);
void
comp_drm_readback_destroy(struct comp_drm_readback *r);
