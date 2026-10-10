// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "comp_drm_dma.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
struct comp_drm_dma
{
	VkDevice device;
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	uint32_t family, pitch, offset;
	int fd;
	bool initialized, external, pending;
};
#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		VkResult result = (call);                                                                              \
		if (result != VK_SUCCESS) {                                                                            \
			fprintf(stderr, "DRM DMA-BUF %s: VkResult=%d\n", #call, result);                               \
			goto fail;                                                                                     \
		}                                                                                                      \
	} while (0)
struct comp_drm_dma *
comp_drm_dma_create(VkPhysicalDevice physical,
                    VkDevice device,
                    uint32_t family,
                    uint32_t width,
                    uint32_t height,
                    VkFormat format,
                    VkImageUsageFlags usage)
{
	struct comp_drm_dma *d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->fd = -1;
	d->device = device;
	d->family = family;
	/* Query the exact renderer usage, including STORAGE for the compute path.
	 * COLOR_ATTACHMENT-only support does not establish STORAGE support. */
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
	    .drmFormatModifier = 0,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkPhysicalDeviceExternalImageFormatInfo external = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
	    .pNext = &modifier,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkPhysicalDeviceImageFormatInfo2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
	                                          .pNext = &external,
	                                          .format = format,
	                                          .type = VK_IMAGE_TYPE_2D,
	                                          .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	                                          .usage = usage};
	VkExternalImageFormatProperties shared = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &shared};
	VkResult qr = vkGetPhysicalDeviceImageFormatProperties2(physical, &query, &props);
	fprintf(stderr, "DMA-BUF format=%d modifier=LINEAR usage=0x%x query=%d external_features=0x%x extent=%ux%u\n",
	        format, usage, qr, shared.externalMemoryProperties.externalMemoryFeatures, width, height);
	if (qr != VK_SUCCESS ||
	    !(shared.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
	    width > props.imageFormatProperties.maxExtent.width ||
	    height > props.imageFormatProperties.maxExtent.height)
		goto fail;
	VkDrmFormatModifierPropertiesListEXT modifiers = {
	    .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
	VkFormatProperties2 formats = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &modifiers};
	vkGetPhysicalDeviceFormatProperties2(physical, format, &formats);
	modifiers.pDrmFormatModifierProperties =
	    calloc(modifiers.drmFormatModifierCount, sizeof(VkDrmFormatModifierPropertiesEXT));
	if (!modifiers.pDrmFormatModifierProperties)
		goto fail;
	vkGetPhysicalDeviceFormatProperties2(physical, format, &formats);
	bool single_plane_linear = false;
	for (uint32_t i = 0; i < modifiers.drmFormatModifierCount; i++)
		if (modifiers.pDrmFormatModifierProperties[i].drmFormatModifier == 0 &&
		    modifiers.pDrmFormatModifierProperties[i].drmFormatModifierPlaneCount == 1)
			single_plane_linear = true;
	free(modifiers.pDrmFormatModifierProperties);
	if (!single_plane_linear) {
		fprintf(stderr, "DMA-BUF requires a reported single-plane LINEAR modifier; no layout guessing\n");
		goto fail;
	}
	uint64_t linear = 0;
	VkImageDrmFormatModifierListCreateInfoEXT list = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
	    .drmFormatModifierCount = 1,
	    .pDrmFormatModifiers = &linear};
	VkExternalMemoryImageCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	                                      .pNext = &list,
	                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                           .pNext = &ei,
	                           .imageType = VK_IMAGE_TYPE_2D,
	                           .format = format,
	                           .extent = {width, height, 1},
	                           .mipLevels = 1,
	                           .arrayLayers = 1,
	                           .samples = VK_SAMPLE_COUNT_1_BIT,
	                           .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	                           .usage = usage,
	                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	CHECK(vkCreateImage(device, &image, NULL, &d->image));
	PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier =
	    (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
	        device, "vkGetImageDrmFormatModifierPropertiesEXT");
	PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
	if (!get_modifier || !get_fd)
		goto fail;
	VkImageDrmFormatModifierPropertiesEXT actual = {.sType =
	                                                    VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
	CHECK(get_modifier(device, d->image, &actual));
	if (actual.drmFormatModifier != linear)
		goto fail;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, d->image, &req);
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(physical, &memory);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
		if ((req.memoryTypeBits & (1u << i)) &&
		    (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			type = i;
			break;
		}
	if (type == UINT32_MAX)
		goto fail;
	/* Always dedicated: also satisfies DEDICATED_ONLY when reported. */
	VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
	                                           .image = d->image};
	VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
	                                     .pNext = &dedicated,
	                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                              .pNext = &export,
	                              .allocationSize = req.size,
	                              .memoryTypeIndex = type};
	CHECK(vkAllocateMemory(device, &alloc, NULL, &d->memory));
	CHECK(vkBindImageMemory(device, d->image, d->memory, 0));
	VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT};
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(device, d->image, &sub, &layout);
	if (layout.rowPitch > UINT32_MAX || layout.offset > UINT32_MAX || layout.rowPitch < (uint64_t)width * 4)
		goto fail;
	d->pitch = layout.rowPitch;
	d->offset = layout.offset;
	VkMemoryGetFdInfoKHR fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
	                           .memory = d->memory,
	                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	CHECK(get_fd(device, &fi, &d->fd));
	VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                              .image = d->image,
	                              .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                              .format = format,
	                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	CHECK(vkCreateImageView(device, &view, NULL, &d->view));
	VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                                .queueFamilyIndex = family};
	CHECK(vkCreateCommandPool(device, &pool, NULL, &d->pool));
	VkCommandBufferAllocateInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                   .commandPool = d->pool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                   .commandBufferCount = 1};
	CHECK(vkAllocateCommandBuffers(device, &cmd, &d->cmd));
	VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CHECK(vkCreateFence(device, &fence, NULL, &d->fence));
	fprintf(stderr, "DMA-BUF export succeeded: pitch=%u offset=%u; no staging buffer or CPU pixel copy\n", d->pitch,
	        d->offset);
	return d;
fail:
	comp_drm_dma_destroy(d);
	return NULL;
}
static VkResult
ownership(struct comp_drm_dma *d, VkQueue queue, VkSemaphore wait, bool release, bool clear)
{
	if (d->pending || (release ? (!d->initialized || d->external) : (d->initialized && !d->external)))
		return VK_ERROR_INITIALIZATION_FAILED;
	CHECK(vkResetCommandBuffer(d->cmd, 0));
	CHECK(vkResetFences(d->device, 1, &d->fence));
	VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	CHECK(vkBeginCommandBuffer(d->cmd, &begin));
	VkImageMemoryBarrier barrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .oldLayout = d->initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .srcQueueFamilyIndex = !d->initialized ? VK_QUEUE_FAMILY_IGNORED
	                           : release       ? d->family
	                                           : VK_QUEUE_FAMILY_FOREIGN_EXT,
	    .dstQueueFamilyIndex = !d->initialized ? VK_QUEUE_FAMILY_IGNORED
	                           : release       ? VK_QUEUE_FAMILY_FOREIGN_EXT
	                                           : d->family,
	    .srcAccessMask = release ? VK_ACCESS_MEMORY_WRITE_BIT : 0,
	    .dstAccessMask = release ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	    .image = d->image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	if (clear) {
		VkClearColorValue color = {.float32 = {0.0f, 0.7f, 0.2f, 1.0f}};
		vkCmdClearColorImage(d->cmd, d->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &barrier.subresourceRange);
	}
	vkCmdPipelineBarrier(d->cmd, release ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     release ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
	                     NULL, 0, NULL, 1, &barrier);
	CHECK(vkEndCommandBuffer(d->cmd));
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	                       .waitSemaphoreCount = wait ? 1 : 0,
	                       .pWaitSemaphores = &wait,
	                       .pWaitDstStageMask = &stage,
	                       .commandBufferCount = 1,
	                       .pCommandBuffers = &d->cmd};
	CHECK(vkQueueSubmit(queue, 1, &submit, d->fence));
	d->pending = true;
	VkResult ret = vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_C(3000000000));
	if (ret != VK_SUCCESS)
		return ret; /* Never reset/reuse in-flight commands. */
	d->pending = false;
	d->initialized = true;
	d->external = release;
	return VK_SUCCESS;
fail:
	return VK_ERROR_INITIALIZATION_FAILED;
}
VkResult
comp_drm_dma_acquire(struct comp_drm_dma *d, VkQueue q)
{
	return ownership(d, q, VK_NULL_HANDLE, false, false);
}
VkResult
comp_drm_dma_release(struct comp_drm_dma *d, VkQueue q, VkSemaphore wait, bool clear)
{
	return ownership(d, q, wait, true, clear);
}
VkImage
comp_drm_dma_image(struct comp_drm_dma *d)
{
	return d->image;
}
VkImageView
comp_drm_dma_view(struct comp_drm_dma *d)
{
	return d->view;
}
int
comp_drm_dma_fd(struct comp_drm_dma *d)
{
	return d->fd;
}
uint32_t
comp_drm_dma_pitch(struct comp_drm_dma *d)
{
	return d->pitch;
}
uint32_t
comp_drm_dma_offset(struct comp_drm_dma *d)
{
	return d->offset;
}
void
comp_drm_dma_destroy(struct comp_drm_dma *d)
{
	if (!d)
		return;
	if (d->pending)
		vkDeviceWaitIdle(d->device);
	if (d->fd >= 0)
		close(d->fd);
	if (d->fence)
		vkDestroyFence(d->device, d->fence, NULL);
	if (d->pool)
		vkDestroyCommandPool(d->device, d->pool, NULL);
	if (d->view)
		vkDestroyImageView(d->device, d->view, NULL);
	if (d->image)
		vkDestroyImage(d->device, d->image, NULL);
	if (d->memory)
		vkFreeMemory(d->device, d->memory, NULL);
	free(d);
}
