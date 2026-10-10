// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#define _POSIX_C_SOURCE 200809L
#include "comp_drm_kms.h"
#include "comp_drm_readback.h"
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static volatile sig_atomic_t stop;
static void
interrupted(int sig)
{
	(void)sig;
	stop = 1;
}
static void
usage(void)
{
	puts(
	    "monado-kms-probe [--device /dev/dri/cardN] --list\n"
	    "  --connector NAME --mode WIDTHxHEIGHT@HZ [--plane ID] --test-only\n"
	    "  --connector NAME --mode WIDTHxHEIGHT@HZ --show [--seconds N]\n"
	    "  --vulkan-list\n"
	    "  --vulkan-readback-test --gpu INDEX (no KMS; CPU allowed for this test only)\n"
	    "  --connector NAME --mode WIDTHxHEIGHT@HZ --vulkan-show [--gpu INDEX] [--seconds N]\n"
	    "Mode refresh must match --list (three decimal places). Default: enumeration only.\n"
	    "Only --show and --vulkan-show apply modesets. CPU Vulkan devices are rejected for display.");
}
static void
modifier_queries(VkPhysicalDevice physical)
{
	VkDrmFormatModifierPropertiesListEXT list = {.sType =
	                                                 VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
	VkFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
	vkGetPhysicalDeviceFormatProperties2(physical, VK_FORMAT_B8G8R8A8_UNORM, &props);
	list.pDrmFormatModifierProperties =
	    calloc(list.drmFormatModifierCount, sizeof(*list.pDrmFormatModifierProperties));
	if (!list.pDrmFormatModifierProperties)
		return;
	vkGetPhysicalDeviceFormatProperties2(physical, VK_FORMAT_B8G8R8A8_UNORM, &props);
	for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
		const VkDrmFormatModifierPropertiesEXT *m = &list.pDrmFormatModifierProperties[i];
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		    .drmFormatModifier = m->drmFormatModifier,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
		VkPhysicalDeviceExternalImageFormatInfo external = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		    .pNext = &modifier,
		    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
		VkPhysicalDeviceImageFormatInfo2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		                                         .pNext = &external,
		                                         .format = VK_FORMAT_B8G8R8A8_UNORM,
		                                         .type = VK_IMAGE_TYPE_2D,
		                                         .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		                                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
		VkExternalImageFormatProperties shared = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
		VkImageFormatProperties2 out = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &shared};
		VkResult ret = vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &out);
		printf(
		    "  BGRA8 modifier=0x%016llx planes=%u features=0x%x dma-buf image query=%d external_features=0x%x "
		    "max=%ux%u (scanout unverified)\n",
		    (unsigned long long)m->drmFormatModifier, m->drmFormatModifierPlaneCount,
		    m->drmFormatModifierTilingFeatures, ret, shared.externalMemoryProperties.externalMemoryFeatures,
		    out.imageFormatProperties.maxExtent.width, out.imageFormatProperties.maxExtent.height);
	}
	free(list.pDrmFormatModifierProperties);
}
static bool
number(const char *s, unsigned long limit, unsigned long *value)
{
	char *end = NULL;
	errno = 0;
	if (!s || !*s || *s == '-')
		return false;
	*value = strtoul(s, &end, 10);
	return !errno && end && !*end && *value <= limit;
}
static int
vulkan(struct comp_drm_kms *k, int selected, bool show)
{
	VkInstance instance = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	struct comp_drm_readback *r = NULL;
	int result = 1;
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	                         .pApplicationName = "monado-kms-probe",
	                         .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkResult ret = vkCreateInstance(&ic, NULL, &instance);
	if (ret) {
		fprintf(stderr, "vkCreateInstance: %d\n", ret);
		return 1;
	}
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, NULL);
	VkPhysicalDevice *ps = calloc(count, sizeof(*ps));
	if (!ps)
		goto end;
	vkEnumeratePhysicalDevices(instance, &count, ps);
	int gpu = -1;
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties prop;
		vkGetPhysicalDeviceProperties(ps[i], &prop);
		printf("Vulkan GPU %u: %s vendor=0x%x type=%u api=%u.%u.%u\n", i, prop.deviceName, prop.vendorID,
		       prop.deviceType, VK_VERSION_MAJOR(prop.apiVersion), VK_VERSION_MINOR(prop.apiVersion),
		       VK_VERSION_PATCH(prop.apiVersion));
		uint32_t n = 0;
		vkEnumerateDeviceExtensionProperties(ps[i], NULL, &n, NULL);
		VkExtensionProperties *es = calloc(n, sizeof(*es));
		if (!es)
			continue;
		vkEnumerateDeviceExtensionProperties(ps[i], NULL, &n, es);
		bool modifiers = false, dma_buf = false;
		for (uint32_t j = 0; j < n; j++) {
			if (!strcmp(es[j].extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME))
				modifiers = true;
			if (!strcmp(es[j].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME))
				dma_buf = true;
			if (strstr(es[j].extensionName, "external_memory") ||
			    strstr(es[j].extensionName, "drm_format_modifier"))
				printf("  device extension: %s\n", es[j].extensionName);
		}
		if (modifiers && dma_buf)
			modifier_queries(ps[i]);
		free(es);
		if ((selected < 0 && gpu < 0 && prop.vendorID == 0x13b5) || selected == (int)i)
			gpu = i;
	}
	if (!show) {
		result = 0;
		free(ps);
		goto end;
	}
	if (gpu < 0) {
		fprintf(stderr, "No selected ARM GPU; use --gpu with the real GPU index.\n");
		free(ps);
		goto end;
	}
	VkPhysicalDevice physical = ps[gpu];
	free(ps);
	VkPhysicalDeviceProperties prop;
	vkGetPhysicalDeviceProperties(physical, &prop);
	if (k && prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
		fprintf(stderr, "Refusing CPU Vulkan device for physical display test\n");
		goto end;
	}
	printf("Selected GPU: %s; sharing=CPU readback (no dma-buf export claimed)\n", prop.deviceName);
	uint32_t n = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
	VkQueueFamilyProperties *qs = calloc(n, sizeof(*qs));
	if (!qs)
		goto end;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, qs);
	uint32_t family = n;
	for (uint32_t i = 0; i < n; i++)
		if (qs[i].queueCount && (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
			family = i;
			break;
		}
	free(qs);
	if (family == n)
		goto end;
	float priority = 1;
	VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                             .queueFamilyIndex = family,
	                             .queueCount = 1,
	                             .pQueuePriorities = &priority};
	VkDeviceCreateInfo dc = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
	ret = vkCreateDevice(physical, &dc, NULL, &device);
	if (ret) {
		fprintf(stderr, "vkCreateDevice: %d\n", ret);
		goto end;
	}
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);
	r = comp_drm_readback_create(physical, device, family, (k ? comp_drm_kms_width(k) : 64),
	                             (k ? comp_drm_kms_height(k) : 64), VK_FORMAT_B8G8R8A8_UNORM,
	                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	if (!r)
		goto end;
	const uint8_t *pixels = NULL;
	struct timespec begin, endtime;
	clock_gettime(CLOCK_MONOTONIC, &begin);
	ret = comp_drm_readback_copy(r, queue, VK_NULL_HANDLE, true, &pixels);
	clock_gettime(CLOCK_MONOTONIC, &endtime);
	printf("Vulkan clear + readback: VkResult=%d time=%.3f ms\n", ret,
	       (endtime.tv_sec - begin.tv_sec) * 1000.0 + (endtime.tv_nsec - begin.tv_nsec) / 1e6);
	if (ret == VK_SUCCESS && k && comp_drm_kms_present(k, pixels, comp_drm_kms_width(k) * 4))
		result = 0;
	if (!k && ret == VK_SUCCESS) {
		bool valid = true;
		for (unsigned i = 0; i < 64 * 64; i++)
			if (pixels[i * 4] != 51 || pixels[i * 4 + 1] < 178 || pixels[i * 4 + 1] > 179 ||
			    pixels[i * 4 + 2] != 0 || pixels[i * 4 + 3] != 255)
				valid = false;
		VkSemaphore sem = VK_NULL_HANDLE;
		VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		ret = vkCreateSemaphore(device, &sc, NULL, &sem);
		for (unsigned frame = 0; frame < 3 && ret == VK_SUCCESS && valid; frame++) {
			VkSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			                       .signalSemaphoreCount = 1,
			                       .pSignalSemaphores = &sem};
			ret = vkQueueSubmit(queue, 1, &signal, VK_NULL_HANDLE);
			if (ret == VK_SUCCESS)
				ret = comp_drm_readback_copy(r, queue, sem, false, &pixels);
			if (ret == VK_SUCCESS)
				for (unsigned i = 0; i < 64 * 64; i++)
					if (pixels[i * 4] != 51 || pixels[i * 4 + 1] < 178 || pixels[i * 4 + 1] > 179 ||
					    pixels[i * 4 + 2] != 0 || pixels[i * 4 + 3] != 255)
						valid = false;
		}
		vkDeviceWaitIdle(device);
		if (sem)
			vkDestroySemaphore(device, sem, NULL);
		printf("Readback pixel verification and binary semaphore reuse: %s VkResult=%d (64x64, four copies)\n",
		       valid && ret == VK_SUCCESS ? "PASS" : "FAIL", ret);
		if (valid && ret == VK_SUCCESS)
			result = 0;
	}
end:
	comp_drm_readback_destroy(r);
	if (device)
		vkDestroyDevice(device, NULL);
	vkDestroyInstance(instance, NULL);
	return result;
}
int
main(int argc, char **argv)
{
	const char *device = NULL, *connector = NULL, *mode = NULL;
	uint32_t plane = 0;
	bool test = false, show = false, vs = false, vl = false, selftest = false;
	int gpu = -1, seconds = 5;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--help")) {
			usage();
			return 0;
		}
		if (!strcmp(a, "--list"))
			continue;
		if (!strcmp(a, "--test-only")) {
			test = true;
			continue;
		}
		if (!strcmp(a, "--show")) {
			show = true;
			continue;
		}
		if (!strcmp(a, "--vulkan-show")) {
			vs = true;
			continue;
		}
		if (!strcmp(a, "--vulkan-readback-test")) {
			selftest = true;
			continue;
		}
		if (!strcmp(a, "--vulkan-list")) {
			vl = true;
			continue;
		}
		if (i + 1 >= argc) {
			usage();
			return 2;
		}
		const char *value = argv[++i];
		unsigned long parsed = 0;
		if (!strcmp(a, "--device"))
			device = value;
		else if (!strcmp(a, "--connector"))
			connector = value;
		else if (!strcmp(a, "--mode"))
			mode = value;
		else if (!strcmp(a, "--plane")) {
			if (!number(value, UINT32_MAX, &parsed) || !parsed)
				return 2;
			plane = parsed;
		} else if (!strcmp(a, "--gpu")) {
			if (!number(value, INT_MAX, &parsed))
				return 2;
			gpu = parsed;
		} else if (!strcmp(a, "--seconds")) {
			if (!number(value, 60, &parsed) || !parsed)
				return 2;
			seconds = parsed;
		} else {
			usage();
			return 2;
		}
	}
	if (selftest) {
		if (test || show || vs || vl)
			return 2;
		return vulkan(NULL, gpu, true);
	}
	if (vl && !test && !show && !vs)
		return vulkan(NULL, gpu, false);
	if ((test + show + vs) > 1 || (vl && (test || show || vs)) || seconds < 1 || seconds > 60 ||
	    ((test || show || vs) && (!connector || !mode))) {
		usage();
		return 2;
	}
	struct comp_drm_kms *k = comp_drm_kms_open(device, stderr);
	if (!k)
		return 1;
	int result = 0;
	if (!test && !show && !vs) {
		comp_drm_kms_list(k);
		goto end;
	}
	if (!comp_drm_kms_prepare(k, connector, mode, plane)) {
		result = 1;
		goto end;
	}
	if (test)
		goto end;
	/* Bounded output, SIGINT/SIGTERM handled outside signal context for restoration. */
	struct sigaction sa = {.sa_handler = interrupted};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	if (vs)
		result = vulkan(k, gpu, true);
	else {
		uint32_t w = comp_drm_kms_width(k), h = comp_drm_kms_height(k);
		uint32_t *pixels = calloc((size_t)w * h, 4);
		if (!pixels) {
			result = 1;
			goto end;
		}
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				pixels[(size_t)y * w + x] = (x / 64 + y / 64) % 2 ? 0x00ff8000 : 0x000040ff;
		if (!comp_drm_kms_present(k, (uint8_t *)pixels, w * 4))
			result = 1;
		free(pixels);
	}
	if (!result)
		for (int i = 0; i < seconds * 10 && !stop; i++) {
			struct timespec delay = {.tv_nsec = 100000000};
			nanosleep(&delay, NULL);
		}
end:
	comp_drm_kms_close(k);
	return result;
}
