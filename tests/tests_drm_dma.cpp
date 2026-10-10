// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
extern "C" {
#include "comp_drm_dma.h"
}
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

/* GPU-free fault injection: validates the ownership/synchronization contract,
 * FD ownership and timeout behavior. This does not emulate driver scanout. */
static VkImageMemoryBarrier last_barrier;
static VkSemaphore waited;
static VkResult wait_result;
static unsigned resets, destroyed, idle, fence_waits;
static VkCommandBuffer reset_cmd;
static int exported_fd;
static bool exportable;
template <typename T>
static T
handle()
{
	return (T)(uintptr_t)1;
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice,
                                          const VkPhysicalDeviceImageFormatInfo2 *info,
                                          VkImageFormatProperties2 *out)
{
	REQUIRE(info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
	REQUIRE(info->usage & VK_IMAGE_USAGE_STORAGE_BIT);
	auto *external = (VkExternalImageFormatProperties *)out->pNext;
	external->externalMemoryProperties.externalMemoryFeatures =
	    exportable ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT : 0;
	out->imageFormatProperties.maxExtent = {4096, 4096, 1};
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *out)
{
	auto *list = (VkDrmFormatModifierPropertiesListEXT *)out->pNext;
	list->drmFormatModifierCount = 1;
	if (list->pDrmFormatModifierProperties)
		list->pDrmFormatModifierProperties[0] = {0, 1, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT};
}
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *out)
{
	*out = handle<VkImage>();
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *out)
{
	*out = {16384, 64, 1};
}
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *out)
{
	*out = {};
	out->memoryTypeCount = 1;
	out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *out)
{
	*out = handle<VkDeviceMemory>();
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)
{
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkGetImageSubresourceLayout(VkDevice, VkImage, const VkImageSubresource *sub, VkSubresourceLayout *out)
{
	REQUIRE(sub->aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT);
	*out = {};
	out->rowPitch = 256;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *out)
{
	*out = handle<VkImageView>();
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *out)
{
	*out = handle<VkCommandPool>();
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *out)
{
	out[0] = handle<VkCommandBuffer>();
	out[1] = (VkCommandBuffer)(uintptr_t)2;
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *out)
{
	*out = handle<VkFence>();
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandBuffer(VkCommandBuffer cmd, VkCommandBufferResetFlags)
{
	resets++;
	reset_cmd = cmd;
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkResetFences(VkDevice, uint32_t, const VkFence *)
{
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *)
{
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkCmdClearColorImage(
    VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *)
{}
VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(VkCommandBuffer,
                     VkPipelineStageFlags,
                     VkPipelineStageFlags,
                     VkDependencyFlags,
                     uint32_t,
                     const VkMemoryBarrier *,
                     uint32_t,
                     const VkBufferMemoryBarrier *,
                     uint32_t count,
                     const VkImageMemoryBarrier *barriers)
{
	REQUIRE(count == 1);
	last_barrier = barriers[0];
}
VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer)
{
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *info, VkFence)
{
	waited = info->waitSemaphoreCount ? info->pWaitSemaphores[0] : VK_NULL_HANDLE;
	return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t timeout)
{
	REQUIRE(timeout == 3000000000);
	fence_waits++;
	return wait_result;
}
VKAPI_ATTR VkResult VKAPI_CALL
vkDeviceWaitIdle(VkDevice)
{
	idle++;
	return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *)
{}
VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *)
{}
VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *)
{}
VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *)
{
	destroyed++;
}
VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *)
{}
}
static VKAPI_ATTR VkResult VKAPI_CALL
get_modifier(VkDevice, VkImage, VkImageDrmFormatModifierPropertiesEXT *out)
{
	out->drmFormatModifier = 0;
	return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL
get_fd(VkDevice, const VkMemoryGetFdInfoKHR *, int *out)
{
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	close(fds[1]);
	*out = exported_fd = fds[0];
	return VK_SUCCESS;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice, const char *name)
{
	return !strcmp(name, "vkGetMemoryFdKHR") ? (PFN_vkVoidFunction)get_fd : (PFN_vkVoidFunction)get_modifier;
}
static comp_drm_dma *
create()
{
	wait_result = VK_SUCCESS;
	exportable = true;
	resets = destroyed = idle = fence_waits = 0;
	return comp_drm_dma_create(handle<VkPhysicalDevice>(), handle<VkDevice>(), 3, 64, 64, VK_FORMAT_B8G8R8A8_UNORM,
	                           VK_IMAGE_USAGE_STORAGE_BIT);
}
TEST_CASE("DMA-BUF ownership handoff and FD cleanup", "[drm][dma]")
{
	auto *d = create();
	REQUIRE(d);
	REQUIRE(comp_drm_dma_pitch(d) == 256);
	REQUIRE(comp_drm_dma_acquire(d, handle<VkQueue>()) == VK_SUCCESS);
	REQUIRE(last_barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
	REQUIRE(fence_waits == 0);
	REQUIRE(reset_cmd == handle<VkCommandBuffer>());
	REQUIRE(comp_drm_dma_acquire(d, handle<VkQueue>()) != VK_SUCCESS);
	REQUIRE(comp_drm_dma_release(d, handle<VkQueue>(), handle<VkSemaphore>(), false) == VK_SUCCESS);
	REQUIRE(waited == handle<VkSemaphore>());
	REQUIRE(fence_waits == 1);
	REQUIRE(reset_cmd == (VkCommandBuffer)(uintptr_t)2);
	REQUIRE(last_barrier.srcQueueFamilyIndex == 3);
	REQUIRE(last_barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);
	REQUIRE(comp_drm_dma_release(d, handle<VkQueue>(), VK_NULL_HANDLE, false) != VK_SUCCESS);
	REQUIRE(comp_drm_dma_acquire(d, handle<VkQueue>()) == VK_SUCCESS);
	REQUIRE(last_barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT);
	REQUIRE(last_barrier.dstQueueFamilyIndex == 3);
	comp_drm_dma_destroy(d);
	REQUIRE(fcntl(exported_fd, F_GETFD) == -1);
	REQUIRE(destroyed == 1);
}
TEST_CASE("DMA-BUF GPU timeout prohibits command and buffer reuse", "[drm][dma]")
{
	auto *d = create();
	REQUIRE(d);
	REQUIRE(comp_drm_dma_acquire(d, handle<VkQueue>()) == VK_SUCCESS);
	wait_result = VK_TIMEOUT;
	REQUIRE(comp_drm_dma_release(d, handle<VkQueue>(), VK_NULL_HANDLE, false) == VK_TIMEOUT);
	auto old_resets = resets;
	REQUIRE(comp_drm_dma_acquire(d, handle<VkQueue>()) != VK_SUCCESS);
	REQUIRE(resets == old_resets);
	comp_drm_dma_destroy(d);
	REQUIRE(idle == 1);
	REQUIRE(fcntl(exported_fd, F_GETFD) == -1);
}
TEST_CASE("DMA-BUF unsupported export refuses allocation", "[drm][dma]")
{
	exportable = false;
	destroyed = 0;
	REQUIRE(comp_drm_dma_create(handle<VkPhysicalDevice>(), handle<VkDevice>(), 3, 64, 64, VK_FORMAT_B8G8R8A8_UNORM,
	                            VK_IMAGE_USAGE_STORAGE_BIT) == nullptr);
	REQUIRE(destroyed == 0);
}
