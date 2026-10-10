// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "comp_drm_readback.h"
#include <stdio.h>
#include <stdlib.h>
struct comp_drm_readback
{
	VkDevice device;
	VkImage image;
	VkImageView view;
	VkDeviceMemory image_memory;
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	uint32_t width, height;
	void *map;
	bool coherent, submitted;
};
static bool
memory_type(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *out, bool *coherent)
{
	VkPhysicalDeviceMemoryProperties m;
	vkGetPhysicalDeviceMemoryProperties(p, &m);
	for (uint32_t i = 0; i < m.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags) {
			*out = i;
			if (coherent)
				*coherent =
				    (m.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
			return true;
		}
	return false;
}
#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		VkResult ret = (call);                                                                                 \
		if (ret != VK_SUCCESS) {                                                                               \
			fprintf(stderr, "DRM readback %s: VkResult=%d\n", #call, ret);                                 \
			goto fail;                                                                                     \
		}                                                                                                      \
	} while (0)
struct comp_drm_readback *
comp_drm_readback_create(VkPhysicalDevice physical,
                         VkDevice device,
                         uint32_t family,
                         uint32_t width,
                         uint32_t height,
                         VkFormat format,
                         VkImageUsageFlags usage)
{
	struct comp_drm_readback *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->device = device;
	r->width = width;
	r->height = height;
	VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                           .imageType = VK_IMAGE_TYPE_2D,
	                           .format = format,
	                           .extent = {width, height, 1},
	                           .mipLevels = 1,
	                           .arrayLayers = 1,
	                           .samples = VK_SAMPLE_COUNT_1_BIT,
	                           .tiling = VK_IMAGE_TILING_OPTIMAL,
	                           .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	CHECK(vkCreateImage(device, &image, NULL, &r->image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, r->image, &req);
	uint32_t type;
	if (!memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type, NULL))
		goto fail;
	VkMemoryAllocateInfo allocation = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
	CHECK(vkAllocateMemory(device, &allocation, NULL, &r->image_memory));
	CHECK(vkBindImageMemory(device, r->image, r->image_memory, 0));
	VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                              .image = r->image,
	                              .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                              .format = format,
	                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	CHECK(vkCreateImageView(device, &view, NULL, &r->view));
	VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                             .size = (VkDeviceSize)width * height * 4,
	                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	CHECK(vkCreateBuffer(device, &buffer, NULL, &r->buffer));
	vkGetBufferMemoryRequirements(device, r->buffer, &req);
	if (!memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &type, &r->coherent))
		goto fail;
	allocation.allocationSize = req.size;
	allocation.memoryTypeIndex = type;
	CHECK(vkAllocateMemory(device, &allocation, NULL, &r->memory));
	CHECK(vkBindBufferMemory(device, r->buffer, r->memory, 0));
	CHECK(vkMapMemory(device, r->memory, 0, VK_WHOLE_SIZE, 0, &r->map));
	VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                                .queueFamilyIndex = family};
	CHECK(vkCreateCommandPool(device, &pool, NULL, &r->pool));
	VkCommandBufferAllocateInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                   .commandPool = r->pool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                   .commandBufferCount = 1};
	CHECK(vkAllocateCommandBuffers(device, &cmd, &r->cmd));
	VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CHECK(vkCreateFence(device, &fence, NULL, &r->fence));
	return r;
fail:
	comp_drm_readback_destroy(r);
	return NULL;
}
VkImage
comp_drm_readback_image(struct comp_drm_readback *r)
{
	return r->image;
}
VkImageView
comp_drm_readback_view(struct comp_drm_readback *r)
{
	return r->view;
}
VkResult
comp_drm_readback_copy(struct comp_drm_readback *r, VkQueue queue, VkSemaphore wait, bool clear, const uint8_t **pixels)
{
	VkDevice device = r->device;
	VkResult ret;
	/* A timeout leaves GPU resources in flight: do not reset or reuse them. */
	if (r->submitted)
		return VK_NOT_READY;
	CHECK(vkResetCommandBuffer(r->cmd, 0));
	CHECK(vkResetFences(device, 1, &r->fence));
	VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	CHECK(vkBeginCommandBuffer(r->cmd, &begin));
	VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                          .srcAccessMask = clear ? 0 : VK_ACCESS_MEMORY_WRITE_BIT,
	                          .dstAccessMask = clear ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
	                          .oldLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
	                          .newLayout = clear ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	                                             : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                          .image = r->image,
	                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
	                     NULL, 1, &b);
	if (clear) {
		VkClearColorValue color = {.float32 = {0.0f, 0.7f, 0.2f, 1.0f}};
		vkCmdClearColorImage(r->cmd, r->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
		                     &b.subresourceRange);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
		                     0, NULL, 1, &b);
	}
	VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	                          .imageExtent = {r->width, r->height, 1}};
	vkCmdCopyImageToBuffer(r->cmd, r->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->buffer, 1, &copy);
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
	                     NULL, 1, &b);
	VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
	                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                              .buffer = r->buffer,
	                              .size = VK_WHOLE_SIZE};
	vkCmdPipelineBarrier(r->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host,
	                     0, NULL);
	CHECK(vkEndCommandBuffer(r->cmd));
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	                       .waitSemaphoreCount = wait ? 1 : 0,
	                       .pWaitSemaphores = &wait,
	                       .pWaitDstStageMask = &stage,
	                       .commandBufferCount = 1,
	                       .pCommandBuffers = &r->cmd};
	CHECK(vkQueueSubmit(queue, 1, &submit, r->fence));
	r->submitted = true;
	ret = vkWaitForFences(device, 1, &r->fence, VK_TRUE, UINT64_C(3000000000));
	if (ret != VK_SUCCESS)
		return ret;
	r->submitted = false;
	if (!r->coherent) {
		VkMappedMemoryRange range = {
		    .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = r->memory, .size = VK_WHOLE_SIZE};
		CHECK(vkInvalidateMappedMemoryRanges(device, 1, &range));
	}
	*pixels = r->map;
	return VK_SUCCESS;
fail:
	return VK_ERROR_INITIALIZATION_FAILED;
}
void
comp_drm_readback_destroy(struct comp_drm_readback *r)
{
	if (!r)
		return;
	VkDevice d = r->device;
	if (r->submitted)
		vkDeviceWaitIdle(d);
	if (r->map)
		vkUnmapMemory(d, r->memory);
	if (r->fence)
		vkDestroyFence(d, r->fence, NULL);
	if (r->pool)
		vkDestroyCommandPool(d, r->pool, NULL);
	if (r->view)
		vkDestroyImageView(d, r->view, NULL);
	if (r->image)
		vkDestroyImage(d, r->image, NULL);
	if (r->image_memory)
		vkFreeMemory(d, r->image_memory, NULL);
	if (r->buffer)
		vkDestroyBuffer(d, r->buffer, NULL);
	if (r->memory)
		vkFreeMemory(d, r->memory, NULL);
	free(r);
}
