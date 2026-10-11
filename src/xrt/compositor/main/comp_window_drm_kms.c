// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "main/comp_window.h"
#include "comp_drm_kms.h"
#include "comp_drm_dma.h"
#include "comp_drm_pipeline.h"
#include "comp_drm_geometry.h"
#include "util/u_pacing.h"
#include "util/u_metrics.h"
#include <stdlib.h>
#include <string.h>
struct drm_kms_target
{
	struct comp_target base;
	struct comp_drm_kms *kms;
	struct comp_drm_dma *dma[COMP_DRM_IMAGES];
	uint64_t frames, gpu_wait_ns, flip_wait_ns;
	struct comp_target_image images[COMP_DRM_IMAGES];
	struct u_pacing_compositor *upc;
	int64_t index, wake_ns, budget_ns;
	unsigned next;
	bool has_init_vulkan, failed, sync_fd, pre_frame_wait;
	FILE *trace;
	int64_t slot_frames[COMP_DRM_IMAGES];
	struct u_metrics_system_present_info prediction, slot_metrics[COMP_DRM_IMAGES];
};
static void
target_trace(struct comp_target *ct, const char *event, int64_t frame_id, int64_t ns, int64_t value)
{
	struct drm_kms_target *t = (void *)ct;
	if (t->trace)
		fprintf(t->trace, "%s,%lld,%lld,%lld\n", event, (long long)frame_id, (long long)ns, (long long)value);
}
static void
kms_observe(void *data, const char *event, int index, uint64_t ns, uint64_t value)
{
	struct drm_kms_target *t = data;
	if (index >= 0 && index < COMP_DRM_IMAGES) {
		target_trace(&t->base, event, t->slot_frames[index], ns, value);
		if (!strcmp(event, "flip") && u_metrics_is_active()) {
			struct u_metrics_system_present_info *info = &t->slot_metrics[index];
			info->actual_present_time_ns = ns;
			info->when_infoed_ns = os_monotonic_get_ns();
			u_metrics_write_system_present_info(info);
		}
	}
}
static bool
target_init_pre_vulkan(struct comp_target *ct)
{
	struct drm_kms_target *t = (void *)ct;
	const char *pre_wait = getenv("XRT_COMPOSITOR_DRM_PRE_FRAME_WAIT");
	if (pre_wait && strcmp(pre_wait, "0") && strcmp(pre_wait, "1")) {
		COMP_ERROR(ct->c, "DRM_PRE_FRAME_WAIT must be 0 or 1");
		return false;
	}
	t->pre_frame_wait = pre_wait && !strcmp(pre_wait, "1");
	if (t->pre_frame_wait && ct->c->settings.use_compute) {
		COMP_ERROR(ct->c, "DRM_PRE_FRAME_WAIT experiment requires XRT_COMPOSITOR_COMPUTE=0");
		return false;
	}
	const char *connector = getenv("XRT_COMPOSITOR_DRM_CONNECTOR"), *mode = getenv("XRT_COMPOSITOR_DRM_MODE");
	if (!connector || !mode || !*connector || !*mode) {
		COMP_ERROR(ct->c,
		           "DRM requires explicit XRT_COMPOSITOR_DRM_CONNECTOR and XRT_COMPOSITOR_DRM_MODE "
		           "(WIDTHxHEIGHT@HZ, three decimals); enumerate with monado-kms-probe --list.");
		return false;
	}
	t->kms = comp_drm_kms_open(getenv("XRT_COMPOSITOR_DRM_DEVICE"), stderr);
	if (!t->kms || !comp_drm_kms_prepare(t->kms, connector, mode, 0))
		return false;
	comp_drm_kms_set_observer(t->kms, kms_observe, t);
	uint32_t w = comp_drm_kms_width(t->kms), h = comp_drm_kms_height(t->kms);
	struct comp_settings *s = &ct->c->settings;
	bool output_profile = comp_drm_arpara_output_viewports(ct->c->xdev, w, h, ct->output_viewports);
	if (output_profile) {
		ct->output_viewport_count = 2;
		COMP_INFO(ct->c,
		          "DRM output geometry: %ux%u, left=(0,0,1920,1920) right=(1920,0,1920,1920); "
		          "application recommended extents unchanged",
		          w, h);
		COMP_WARN(ct->c,
		          "arpara output profile preserves provisional driver SBS ordering; "
		          "physical eye ordering, panel native resolution and optical calibration unverified.");
	} else if ((uint64_t)w * s->preferred.height != (uint64_t)h * s->preferred.width) {
		const char *allow = getenv("XRT_COMPOSITOR_DRM_ALLOW_STRETCH");
		if (!allow || strcmp(allow, "1")) {
			COMP_ERROR(ct->c,
			           "HMD provisional extent %ux%u and KMS mode %ux%u have different aspect ratios. "
			           "Explicit DRM_ALLOW_STRETCH=1 required for diagnostic output.",
			           s->preferred.width, s->preferred.height, w, h);
			return false;
		}
		COMP_WARN(ct->c,
		          "Explicit diagnostic anisotropic scaling %ux%u -> %ux%u; optical calibration unresolved.",
		          s->preferred.width, s->preferred.height, w, h);
	}
	s->preferred.width = w;
	s->preferred.height = h;
	s->nominal_frame_interval_ns = comp_drm_kms_period_ns(t->kms);
	u_pc_destroy(&t->upc);
	u_pc_fake_create(s->nominal_frame_interval_ns, os_monotonic_get_ns(), &t->upc);
	COMP_WARN(ct->c,
	          "DRM/KMS DMA-BUF direct scanout: no CPU pixel copy; queued page-flip, 3 "
	          "images; no low-latency "
	          "claim.");
	return true;
}
static bool
target_init_post_vulkan(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct drm_kms_target *t = (void *)ct;
	struct vk_bundle *vk = &ct->c->base.vk;
	VkPhysicalDeviceProperties p;
	vk->vkGetPhysicalDeviceProperties(vk->physical_device, &p);
	if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
		COMP_ERROR(ct->c, "DRM refuses CPU Vulkan device: %s", p.deviceName);
		return false;
	}
	COMP_INFO(ct->c, "DRM Vulkan GPU: %s", p.deviceName);
	const char *sync = getenv("XRT_COMPOSITOR_DRM_SYNC_FD");
	if (sync && strcmp(sync, "0") && strcmp(sync, "1")) {
		COMP_ERROR(ct->c, "DRM_SYNC_FD must be 0 or 1 (unset selects automatically)");
		return false;
	}
	t->sync_fd = (!sync || strcmp(sync, "0")) && vk->has_KHR_external_fence_fd &&
	             comp_drm_dma_sync_fd_supported(vk->physical_device, vk->device);
	if (t->pre_frame_wait && !t->sync_fd) {
		COMP_ERROR(ct->c, "DRM_PRE_FRAME_WAIT experiment requires Vulkan sync FD handoff");
		return false;
	}
	if (t->pre_frame_wait)
		COMP_WARN(ct->c, "DRM pre-frame flip wait enabled: wait before prediction/pose; "
		          "half-period planning budget, not measured GPU time; experimental");
	if (sync && !strcmp(sync, "1") && !t->sync_fd) {
		COMP_ERROR(ct->c, "Requested sync FD unavailable: plane IN_FENCE_FD=%d, external_fence_fd enabled=%d",
		           comp_drm_kms_has_in_fence(t->kms), vk->has_KHR_external_fence_fd);
		return false;
	}
	t->has_init_vulkan = true;
	return true;
}
static bool
target_check_ready(struct comp_target *ct)
{
	struct drm_kms_target *t = (void *)ct;
	return t->kms && !t->failed;
}
static bool
target_is_shared_presentable_image(struct comp_target *ct)
{
	return false;
}
static void
target_create_images(struct comp_target *ct,
                     const struct comp_target_create_images_info *info,
                     struct vk_bundle_queue *queue)
{
	struct drm_kms_target *t = (void *)ct;
	struct vk_bundle *vk = &ct->c->base.vk;
	if (t->dma[0])
		return;
	if (info->extent.width != comp_drm_kms_width(t->kms) || info->extent.height != comp_drm_kms_height(t->kms)) {
		COMP_ERROR(ct->c, "DRM image extent must match selected physical mode");
		t->failed = true;
		return;
	}
	if (info->format_count > XRT_MAX_SWAPCHAIN_FORMATS) {
		COMP_ERROR(ct->c, "DRM requested format count exceeds array capacity");
		t->failed = true;
		return;
	}
	VkFormat format = VK_FORMAT_UNDEFINED;
	for (uint32_t i = 0; i < info->format_count; i++)
		if (info->formats[i] == VK_FORMAT_B8G8R8A8_UNORM || info->formats[i] == VK_FORMAT_B8G8R8A8_SRGB) {
			format = info->formats[i];
			break;
		}
	if (format == VK_FORMAT_UNDEFINED) {
		COMP_ERROR(ct->c, "DRM DMA-BUF requires requested BGRA8 format");
		t->failed = true;
		return;
	}
	for (unsigned i = 0; i < COMP_DRM_IMAGES; i++) {
		t->dma[i] = comp_drm_dma_create(vk->physical_device, vk->device, queue->family_index,
		                                info->extent.width, info->extent.height, format, info->image_usage);
		if (!t->dma[i] || !comp_drm_kms_import(t->kms, i, comp_drm_dma_fd(t->dma[i]),
		                                       comp_drm_dma_pitch(t->dma[i]), comp_drm_dma_offset(t->dma[i]))) {
			COMP_ERROR(ct->c,
			           "DMA-BUF allocation/import failed; no CPU fallback. See usage/format/IOCTL logs.");
			t->failed = true;
			return;
		}
		if (t->sync_fd && !comp_drm_kms_supports_sync_fd(t->kms, i)) {
			const char *sync = getenv("XRT_COMPOSITOR_DRM_SYNC_FD");
			if (i || t->pre_frame_wait || (sync && !strcmp(sync, "1"))) {
				COMP_ERROR(ct->c, "Requested KMS sync FD handoff unavailable for image %u", i);
				t->failed = true;
				return;
			}
			t->sync_fd = false;
		}
		if (t->sync_fd && !comp_drm_dma_enable_sync_fd(t->dma[i])) {
			COMP_ERROR(ct->c, "Exportable sync FD fence allocation failed");
			t->failed = true;
			return;
		}
		t->images[i].handle = comp_drm_dma_image(t->dma[i]);
		t->images[i].view = comp_drm_dma_view(t->dma[i]);
	}
	COMP_INFO(ct->c, "DRM synchronization: %s",
	          t->sync_fd
	              ? (comp_drm_kms_has_in_fence(t->kms)
	                     ? "Vulkan sync FD -> KMS IN_FENCE_FD; CPU release fence waits=0"
			     : "Vulkan sync FD -> DMA-BUF reservation -> Rockchip KMS; CPU release fence waits=0")
		      : "CPU release fence wait (disabled or unsupported); DMA-BUF scanout retained");
	VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	if (vk->vkCreateSemaphore(vk->device, &sem, NULL, &ct->semaphores.render_complete) != VK_SUCCESS) {
		t->failed = true;
		return;
	}
	ct->image_count = COMP_DRM_IMAGES;
	ct->defer_present_wait = true;
	ct->images = t->images;
	ct->format = format;
	ct->width = info->extent.width;
	ct->height = info->extent.height;
	ct->final_layout = VK_IMAGE_LAYOUT_GENERAL;
	ct->surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	ct->present_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	COMP_INFO(ct->c, "DRM final render extent=%ux%u, framebuffer=%ux%u, output viewports=%u", ct->width, ct->height,
	          comp_drm_kms_width(t->kms), comp_drm_kms_height(t->kms), ct->output_viewport_count);
}
static bool
target_has_images(struct comp_target *ct)
{
	return ct->images != NULL;
}
static VkResult
target_acquire(struct comp_target *ct, uint32_t *index)
{
	struct drm_kms_target *t = (void *)ct;
	if (t->failed || !ct->images || t->index != -1)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (!comp_drm_kms_pump(t->kms, false)) {
		t->failed = true;
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	unsigned candidate;
	for (candidate = 0; candidate < COMP_DRM_IMAGES; candidate++) {
		unsigned slot = (t->next + candidate) % COMP_DRM_IMAGES;
		if (comp_drm_kms_slot_free(t->kms, slot)) {
			t->next = slot;
			break;
		}
	}
	if (candidate == COMP_DRM_IMAGES) {
		t->failed = true;
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	struct vk_bundle_queue *queue = ct->c->base.vk.main_queue;
	/* This slot is neither scanning out nor queued. Acquire FOREIGN ownership
	 * before the renderer touches it, with the same externally locked queue. */
	comp_drm_dma_retire(t->dma[t->next]);
	vk_queue_lock(queue);
	VkResult ret = comp_drm_dma_acquire(t->dma[t->next], queue->queue);
	vk_queue_unlock(queue);
	if (ret != VK_SUCCESS) {
		t->failed = true;
		return ret;
	}
	*index = t->next;
	t->index = t->next;
	return VK_SUCCESS;
}
static VkResult
target_present(struct comp_target *ct,
               struct vk_bundle_queue *queue,
               uint32_t index,
               uint64_t value,
               int64_t desired,
               int64_t slop)
{
	struct drm_kms_target *t = (void *)ct;
	if (t->failed || t->index != (int64_t)index || index >= COMP_DRM_IMAGES)
		return VK_ERROR_INITIALIZATION_FAILED;
	/* Same queue family as rendering. The renderer signals this binary
	 * semaphore once; the ownership-release submission consumes it once. */
	if (queue != ct->c->base.vk.main_queue) {
		t->failed = true;
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	int64_t begin = os_monotonic_get_ns();
	t->slot_frames[index] = ct->c->frame.rendering.id;
	t->slot_metrics[index] = t->prediction;
	t->slot_metrics[index].frame_id = t->slot_frames[index];
	t->slot_metrics[index].desired_present_time_ns = desired;
	t->slot_metrics[index].predicted_display_time_ns = ct->c->frame.rendering.predicted_display_time_ns;
	target_trace(ct, "present_request", t->slot_frames[index], begin, index);
	vk_queue_lock(queue);
	int fd = -1;
	VkResult ret =
	    t->sync_fd ? comp_drm_dma_release_sync_fd(t->dma[index], queue->queue, ct->semaphores.render_complete, &fd)
	               : comp_drm_dma_release(t->dma[index], queue->queue, ct->semaphores.render_complete, false);
	vk_queue_unlock(queue);
	int64_t ready = os_monotonic_get_ns();
	target_trace(ct, "release_return", t->slot_frames[index], ready, t->sync_fd);
	if (ret != VK_SUCCESS || !comp_drm_kms_queue_imported_fenced(t->kms, index, fd)) {
		t->failed = true;
		COMP_ERROR(ct->c, "DRM present failed VkResult=%d; no buffer reuse", ret);
		return ret == VK_SUCCESS ? VK_ERROR_SURFACE_LOST_KHR : ret;
	}
	int64_t sample = ready - t->wake_ns;
	int64_t period = comp_drm_kms_period_ns(t->kms);
	if (!t->sync_fd && t->wake_ns && sample > 0 && sample < period * 4)
		t->budget_ns = t->budget_ns ? (t->budget_ns * 7 + sample + 1000000) / 8 : sample + 1000000;
	t->gpu_wait_ns += ready - begin;
	t->flip_wait_ns += os_monotonic_get_ns() - ready;
	if (++t->frames % 120 == 0) {
		COMP_INFO(ct->c,
		          "DMA-BUF 120 frames: average %s %.3f ms, previous flip drain/commit %.3f ms; "
		          "CPU pixel copies=0; CPU release fence waits=%u; prediction budget=%.3f ms%s",
		          t->sync_fd ? "release submit/export" : "release/GPU wait", t->gpu_wait_ns / 120e6,
		          t->flip_wait_ns / 120e6, t->sync_fd ? 0 : 120,
		          (t->pre_frame_wait ? period / 2 : (t->budget_ns ? t->budget_ns : period)) / 1e6,
		          t->pre_frame_wait ? " (pre-frame wait, half-period allowance, not GPU time)"
		                            : (t->sync_fd ? " (fixed one-period estimate, not GPU time)" : " (wake-to-ready)"));
		t->gpu_wait_ns = t->flip_wait_ns = 0;
	}
	t->index = -1;
	t->next = (index + 1) % COMP_DRM_IMAGES;
	return VK_SUCCESS;
}
static VkResult
target_wait_for_present(struct comp_target *ct, time_duration_ns timeout)
{
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}
static void
target_flush(struct comp_target *ct)
{}
static VkResult
target_update_timings(struct comp_target *ct)
{
	struct drm_kms_target *t = (void *)ct;
	if (!comp_drm_kms_pump(t->kms, false)) {
		t->failed = true;
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	return VK_SUCCESS;
}
static void
target_set_title(struct comp_target *ct, const char *title)
{}
static VkResult
target_queue_supports_present(struct comp_target *ct, struct vk_bundle_queue *queue, VkBool32 *supported)
{
	*supported = queue == ct->c->base.vk.main_queue;
	return VK_SUCCESS;
}
static void
target_destroy(struct comp_target *ct)
{
	struct drm_kms_target *t = (void *)ct;
	struct vk_bundle *vk = &ct->c->base.vk;
	comp_drm_kms_close(t->kms);
	if (t->trace)
		fclose(t->trace);
	if (t->has_init_vulkan) {
		vk->vkDeviceWaitIdle(vk->device);
		for (unsigned i = 0; i < COMP_DRM_IMAGES; i++)
			comp_drm_dma_destroy(t->dma[i]);
		if (ct->semaphores.render_complete)
			vk->vkDestroySemaphore(vk->device, ct->semaphores.render_complete, NULL);
	}
	u_pc_destroy(&t->upc);
	free(t);
}
static void
target_calc_frame_pacing(struct comp_target *ct,
                         int64_t *out_frame_id,
                         int64_t *out_wake_up_time_ns,
                         int64_t *out_desired_present_time_ns,
                         int64_t *out_present_slop_ns,
                         int64_t *out_predicted_display_time_ns)
{
	struct drm_kms_target *dit = (struct drm_kms_target *)ct;

	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t predicted_display_time_ns = 0;
	int64_t predicted_display_period_ns = 0;
	int64_t min_display_period_ns = 0;
	int64_t wait_begin_ns = os_monotonic_get_ns();
	/* Drain on the compositor thread before prediction, layer selection and
	 * timewarp pose fetch. Never wait while holding the Vulkan queue lock.
	 * The existing present-side drain and slot lifetime checks remain intact. */
	if (dit->pre_frame_wait && !dit->failed && !comp_drm_kms_pump(dit->kms, true)) {
		dit->failed = true;
		COMP_ERROR(ct->c, "DRM pre-frame flip wait failed; target disabled, no buffer reuse");
	}
	int64_t now_ns = os_monotonic_get_ns();
	int64_t period = comp_drm_kms_period_ns(dit->kms);
	/* After retiring the previous flip, Graphics can target the next vblank.
	 * This is a conservative planning allowance, not a GPU duration sample.
	 * Late completion is still gated by IN_FENCE_FD and reported by the trace. */
	int64_t budget = dit->pre_frame_wait ? period / 2 : (dit->budget_ns ? dit->budget_ns : period);
	int64_t kms_present = comp_drm_predict_present(now_ns, comp_drm_kms_last_flip_ns(dit->kms), period, budget);
	u_pc_fake_override_next_present(dit->upc, kms_present, now_ns);

	u_pc_predict(dit->upc,                     //
	             now_ns,                       //
	             &frame_id,                    //
	             &wake_up_time_ns,             //
	             &desired_present_time_ns,     //
	             &present_slop_ns,             //
	             &predicted_display_time_ns,   //
	             &predicted_display_period_ns, //
	             &min_display_period_ns);      //

	int64_t offset = predicted_display_time_ns - desired_present_time_ns;
	/* FIFO/backpressure limits work to available scanout slots. Start immediately
	 * instead of imposing an unrelated fake 3ms wake-up budget on a GPU-bound queue. */
	wake_up_time_ns = now_ns;
	predicted_display_time_ns = desired_present_time_ns + offset;
	dit->prediction = (struct u_metrics_system_present_info){.frame_id = frame_id,
	                                                         .when_predict_ns = now_ns,
	                                                         .predicted_wake_up_time_ns = wake_up_time_ns,
	                                                         .desired_present_time_ns = desired_present_time_ns,
	                                                         .predicted_display_time_ns = predicted_display_time_ns,
	                                                         .present_slop_ns = present_slop_ns};
	target_trace(ct, "predict", frame_id, now_ns, period);
	if (dit->pre_frame_wait) {
		target_trace(ct, "pre_frame_wait_begin", frame_id, wait_begin_ns, 0);
		target_trace(ct, "pre_frame_wait_end", frame_id, now_ns, dit->failed);
	}
	target_trace(ct, "desired", frame_id, desired_present_time_ns, budget);
	target_trace(ct, "display_estimate", frame_id, predicted_display_time_ns, offset);
	*out_frame_id = frame_id;
	*out_wake_up_time_ns = wake_up_time_ns;
	*out_desired_present_time_ns = desired_present_time_ns;
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_present_slop_ns = present_slop_ns;
}

static void
target_mark_timing_point(struct comp_target *ct, enum comp_target_timing_point point, int64_t frame_id, int64_t when_ns)
{
	struct drm_kms_target *dit = (struct drm_kms_target *)ct;
	const char *names[] = {"wake", "begin", "submit_begin", "submit_end"};
	/* Explicit switch below retains the target timing contract. */
	if (point >= COMP_TARGET_TIMING_POINT_WAKE_UP && point <= COMP_TARGET_TIMING_POINT_SUBMIT_END)
		target_trace(ct, names[point - COMP_TARGET_TIMING_POINT_WAKE_UP], frame_id, when_ns, 0);

	switch (point) {
	case COMP_TARGET_TIMING_POINT_WAKE_UP:
		dit->prediction.when_woke_ns = when_ns;
		dit->wake_ns = when_ns;
		u_pc_mark_point(dit->upc, U_TIMING_POINT_WAKE_UP, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_BEGIN: //
		dit->prediction.when_began_ns = when_ns;
		u_pc_mark_point(dit->upc, U_TIMING_POINT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN:
		u_pc_mark_point(dit->upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_END:
		dit->prediction.when_submitted_ns = when_ns;
		u_pc_mark_point(dit->upc, U_TIMING_POINT_SUBMIT_END, frame_id, when_ns);
		break;
	default: assert(false);
	}
}

static void
target_info_gpu(struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{
	struct drm_kms_target *dit = (struct drm_kms_target *)ct;

	u_pc_info_gpu(dit->upc, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
	target_trace(ct, "gpu_start", frame_id, gpu_start_ns, 0);
	target_trace(ct, "gpu_end", frame_id, gpu_end_ns, 0);
}


static bool
factory_detect(const struct comp_target_factory *f, struct comp_compositor *c)
{
	return false;
}
static bool
factory_create(const struct comp_target_factory *f, struct comp_compositor *c, struct comp_target **out)
{
	struct drm_kms_target *t = calloc(1, sizeof(*t));
	if (!t)
		return false;
	t->base = (struct comp_target){.name = "drm_kms",
	                               .c = c,
	                               .init_pre_vulkan = target_init_pre_vulkan,
	                               .init_post_vulkan = target_init_post_vulkan,
	                               .check_ready = target_check_ready,
	                               .is_shared_presentable_image = target_is_shared_presentable_image,
	                               .create_images = target_create_images,
	                               .has_images = target_has_images,
	                               .acquire = target_acquire,
	                               .present = target_present,
	                               .wait_for_present = target_wait_for_present,
	                               .flush = target_flush,
	                               .calc_frame_pacing = target_calc_frame_pacing,
	                               .mark_timing_point = target_mark_timing_point,
	                               .update_timings = target_update_timings,
	                               .info_gpu = target_info_gpu,
	                               .set_title = target_set_title,
	                               .queue_supports_present = target_queue_supports_present,
	                               .destroy = target_destroy};
	t->index = -1;
	const char *trace_path = getenv("XRT_COMPOSITOR_DRM_TRACE_FILE");
	if (trace_path && *trace_path) {
		t->trace = fopen(trace_path, "w");
		if (!t->trace) {
			COMP_ERROR(c, "Cannot open DRM trace file: %s", trace_path);
			free(t);
			return false;
		}
		setvbuf(t->trace, NULL, _IOFBF, 65536);
		fprintf(t->trace, "event,frame_id,monotonic_ns,value\n");
		t->base.trace_timing = target_trace;
	}
	u_pc_fake_create(c->settings.nominal_frame_interval_ns, os_monotonic_get_ns(), &t->upc);
	*out = &t->base;
	return true;
}
const struct comp_target_factory comp_target_factory_drm_kms = {
    .name = "Native DRM/KMS (DMA-BUF direct scanout)",
    .identifier = "drm_kms",
    .required_instance_version = VK_API_VERSION_1_1,
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .detect = factory_detect,
    .create_target = factory_create,
};
