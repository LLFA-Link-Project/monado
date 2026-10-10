// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
struct comp_drm_dma;
/* Device must enable external_memory_fd, external_memory_dma_buf,
 * image_drm_format_modifier and queue_family_foreign. No pixel mapping/copy. */
struct comp_drm_dma *
comp_drm_dma_create(VkPhysicalDevice physical,
                    VkDevice device,
                    uint32_t family,
                    uint32_t width,
                    uint32_t height,
                    VkFormat format,
                    VkImageUsageFlags usage);
VkImage
comp_drm_dma_image(struct comp_drm_dma *d);
VkImageView
comp_drm_dma_view(struct comp_drm_dma *d);
int
comp_drm_dma_fd(struct comp_drm_dma *d);
uint32_t
comp_drm_dma_pitch(struct comp_drm_dma *d);
uint32_t
comp_drm_dma_offset(struct comp_drm_dma *d);
/* Queue externally locked. Acquire only after the previous scanout has retired.
 * Acquire submits a barrier without CPU waiting on the renderer queue.
 * Release consumes the renderer's binary semaphore and waits for GPU completion.
 * Optional clear is exclusively for the standalone diagnostic. */
VkResult
comp_drm_dma_acquire(struct comp_drm_dma *d, VkQueue queue);
VkResult
comp_drm_dma_release(struct comp_drm_dma *d, VkQueue queue, VkSemaphore wait, bool clear);
void
comp_drm_dma_destroy(struct comp_drm_dma *d);

/* Caller must have enabled VK_KHR_external_fence_fd on this device. */
bool
comp_drm_dma_sync_fd_supported(VkPhysicalDevice physical, VkDevice device);
/* Before first acquire; exportable fence creation failure is fatal to this image. */
bool
comp_drm_dma_enable_sync_fd(struct comp_drm_dma *d);
/* Consumes renderer semaphore once, returns caller-owned sync FD (or -1 for
 * already signaled). No CPU GPU wait. On failure, do not retry or reuse image. */
VkResult
comp_drm_dma_release_sync_fd(struct comp_drm_dma *d, VkQueue queue, VkSemaphore wait, int *fd);
/* Only after KMS has completed the fenced commit AND a subsequent page-flip
 * has removed this image from scanout. Never call after a failed commit. */
void
comp_drm_dma_retire(struct comp_drm_dma *d);
