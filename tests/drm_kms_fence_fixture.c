// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
/* Compile the real implementation with only atomic ioctl entry points replaced.
 * Internal KMS state is constructed here so tests need no card or privileged
 * modeset, and production code needs no test-only constructors. */
#define drmModeAtomicAddProperty fixture_add_property
#define drmModeAtomicCommit fixture_commit
#define ioctl fixture_ioctl
#include "../src/xrt/compositor/main/comp_drm_kms.c"
#undef NDEBUG /* Fault-injection checks must also run in release builds. */
#include <assert.h>
#include <stdarg.h>

static unsigned scenario, commits, imports, input_properties;
static int passed_fd;

int
fixture_add_property(drmModeAtomicReqPtr req, uint32_t object, uint32_t prop, uint64_t value)
{
	(void)req;
	if (prop == 77) {
		assert(object == 39);
		assert((int)value == passed_fd);
		input_properties++;
		if (scenario == 8)
			return -1;
	}
	return 1;
}
int
fixture_commit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *data)
{
	(void)fd;
	(void)req;
	(void)data;
	assert(flags & DRM_MODE_ATOMIC_NONBLOCK);
	assert(flags & DRM_MODE_PAGE_FLIP_EVENT);
	if (passed_fd >= 0)
		assert(fcntl(passed_fd, F_GETFD) >= 0); // FD lives through atomic ioctl.
	commits++;
	if (scenario == 2) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}
int
fixture_ioctl(int fd, unsigned long request, ...)
{
	assert(fd == 123);
#ifdef DMA_BUF_IOCTL_IMPORT_SYNC_FILE
	assert(request == DMA_BUF_IOCTL_IMPORT_SYNC_FILE);
	va_list args;
	va_start(args, request);
	struct dma_buf_import_sync_file *info = va_arg(args, struct dma_buf_import_sync_file *);
	va_end(args);
	assert(info->flags == DMA_BUF_SYNC_WRITE); // Readers must wait for GPU writes.
	assert(info->fd == passed_fd);
	assert(fcntl(info->fd, F_GETFD) >= 0);
	imports++;
	if (scenario == 3) {
		errno = ENOTTY;
		return -1;
	}
	return 0;
#else
	(void)request;
	return -1;
#endif
}
int
drm_kms_fence_fixture(unsigned selected)
{
	scenario = selected;
	commits = imports = input_properties = 0;
	struct comp_drm_kms k = {
	    .fd = -1, .log = stderr, .plane = 39, .flip_done = true, .slots = {-1, -1}, .count = 1};
	k.props[0] = (struct saved_prop){84, 22, 0};
	k.imported[0].fb = 96;
	k.imported[0].dma_fd = 123;
	k.in_fence = 77;
	if (scenario == 1 || scenario == 3 || scenario == 5) {
		k.in_fence = 0;
		k.imported[0].implicit_sync = scenario != 5;
	}
	if (scenario == 7)
		k.slots.scanout = 0;
	if (scenario == 9)
		k.failed = true;
	int fds[2];
	assert(pipe(fds) == 0);
	close(fds[1]);
	passed_fd = fds[0];
	if (scenario == 6) {
		close(passed_fd);
		passed_fd = -1; // Vulkan already-signaled special value.
	}
	bool ok = comp_drm_kms_queue_imported_fenced(&k, scenario == 4 ? COMP_DRM_IMAGES : 0, passed_fd);
	bool expected = scenario == 0 || scenario == 1 || scenario == 6;
	assert(ok == expected);
	assert(k.failed == !ok);
	if (!ok)
		assert(!comp_drm_kms_slot_free(&k, 1));
	if (passed_fd >= 0)
		assert(fcntl(passed_fd, F_GETFD) == -1); // Every success/failure closes FD.
	assert(commits == (expected || scenario == 2 ? 1u : 0u));
	assert(imports == (scenario == 1 || scenario == 3 ? 1u : 0u));
	assert(input_properties == (scenario == 0 || scenario == 2 || scenario == 8 ? 1u : 0u));
	if (ok) {
		assert(!k.flip_done);
		assert(k.slots.queued == 0);
		assert(!comp_drm_kms_slot_free(&k, 0)); // Queued frame cannot be rendered.
	}
	return 0;
}
