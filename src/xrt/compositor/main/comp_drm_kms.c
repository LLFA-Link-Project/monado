// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#define _POSIX_C_SOURCE 200809L
#include "comp_drm_kms.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

struct saved_prop
{
	uint32_t object, property;
	uint64_t value;
};
struct dumb
{
	uint32_t handle, pitch, fb;
	uint64_t size;
	void *map;
};
struct comp_drm_kms
{
	int fd;
	FILE *log;
	drmModeRes *res;
	drmModeModeInfo mode;
	uint32_t connector, crtc, plane, blob;
	struct saved_prop props[16];
	unsigned count;
	struct dumb buffers[2];
	struct
	{
		uint32_t handle, fb;
	} imported[2];
	int scanout;
	unsigned next;
	bool changed, failed, flip_done;
	uint32_t old_blob;
};
static bool
error(struct comp_drm_kms *k, const char *op)
{
	fprintf(k->log, "KMS %s: %s (errno=%d)\n", op, strerror(errno), errno);
	return false;
}
static double
refresh(const drmModeModeInfo *m)
{
	double hz = m->htotal && m->vtotal ? (double)m->clock * 1000 / m->htotal / m->vtotal : 0;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		hz *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		hz /= 2;
	if (m->vscan > 1)
		hz /= m->vscan;
	return hz;
}
static uint32_t
property(struct comp_drm_kms *k, uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
	drmModeObjectProperties *ps = drmModeObjectGetProperties(k->fd, obj, type);
	uint32_t id = 0;
	if (!ps)
		return 0;
	for (uint32_t i = 0; i < ps->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(k->fd, ps->props[i]);
		if (p && !strcmp(p->name, name)) {
			id = p->prop_id;
			if (value)
				*value = ps->prop_values[i];
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(ps);
	return id;
}
struct comp_drm_kms *
comp_drm_kms_open(const char *device, FILE *log)
{
	struct comp_drm_kms *k = calloc(1, sizeof(*k));
	if (!k)
		return NULL;
	k->fd = -1;
	k->scanout = -1;
	k->log = log;
	glob_t paths = {0};
	if (!device)
		glob("/dev/dri/card*", 0, NULL, &paths);
	size_t n = device ? 1 : paths.gl_pathc;
	for (size_t i = 0; i < n; i++) {
		const char *path = device ? device : paths.gl_pathv[i];
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			fprintf(log, "open %s: %s\n", path, strerror(errno));
			continue;
		}
		drmModeRes *r = drmModeGetResources(fd);
		if (!r || !r->count_connectors) {
			if (!r)
				error(k, "drmModeGetResources");
			drmModeFreeResources(r);
			close(fd);
			continue;
		}
		k->fd = fd;
		k->res = r;
		drmVersionPtr v = drmGetVersion(fd);
		fprintf(log, "KMS device=%s driver=%s master=%d\n", path, v ? v->name : "unknown", drmIsMaster(fd));
		drmFreeVersion(v);
		break;
	}
	globfree(&paths);
	if (k->fd < 0) {
		fprintf(log, "No accessible KMS card (check card vs render node and local seat ACL).\n");
		free(k);
		return NULL;
	}
	return k;
}
static void
list_modifiers(struct comp_drm_kms *k, uint32_t plane)
{
	uint64_t id = 0;
	property(k, plane, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &id);
	drmModePropertyBlobRes *b = id ? drmModeGetPropertyBlob(k->fd, id) : NULL;
	if (!b)
		return;
	if (b->length >= sizeof(struct drm_format_modifier_blob)) {
		const struct drm_format_modifier_blob *h = b->data;
		if (h->formats_offset <= b->length && h->modifiers_offset <= b->length &&
		    h->count_formats <= (b->length - h->formats_offset) / sizeof(uint32_t) &&
		    h->count_modifiers <= (b->length - h->modifiers_offset) / sizeof(struct drm_format_modifier)) {
			const uint32_t *fs = (const void *)((const char *)b->data + h->formats_offset);
			const struct drm_format_modifier *ms =
			    (const void *)((const char *)b->data + h->modifiers_offset);
			for (uint32_t i = 0; i < h->count_modifiers; i++)
				for (unsigned j = 0; j < 64; j++) {
					uint64_t f = (uint64_t)ms[i].offset + j;
					if ((ms[i].formats & (UINT64_C(1) << j)) && f < h->count_formats)
						fprintf(k->log, "  format=0x%08x modifier=0x%016llx\n", fs[f],
						        (unsigned long long)ms[i].modifier);
				}
		}
	}
	drmModeFreePropertyBlob(b);
}
void
comp_drm_kms_list(struct comp_drm_kms *k)
{
	for (int i = 0; i < k->res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(k->fd, k->res->connectors[i]);
		if (!c)
			continue;
		fprintf(k->log, "connector %s-%u id=%u connected=%d encoder=%u\n",
		        drmModeGetConnectorTypeName(c->connector_type), c->connector_type_id, c->connector_id,
		        c->connection == DRM_MODE_CONNECTED, c->encoder_id);
		for (int j = 0; j < c->count_modes; j++)
			fprintf(k->log, "  %ux%u@%.3f%s\n", c->modes[j].hdisplay, c->modes[j].vdisplay,
			        refresh(&c->modes[j]), c->modes[j].type & DRM_MODE_TYPE_PREFERRED ? " preferred" : "");
		for (int j = 0; j < c->count_encoders; j++) {
			drmModeEncoder *e = drmModeGetEncoder(k->fd, c->encoders[j]);
			if (e)
				fprintf(k->log, "  encoder=%u crtc=%u possible_crtcs=0x%x\n", e->encoder_id, e->crtc_id,
				        e->possible_crtcs);
			drmModeFreeEncoder(e);
		}
		drmModeFreeConnector(c);
	}
	for (int i = 0; i < k->res->count_crtcs; i++) {
		drmModeCrtc *c = drmModeGetCrtc(k->fd, k->res->crtcs[i]);
		if (c)
			fprintf(k->log, "crtc=%u fb=%u active_mode=%d\n", c->crtc_id, c->buffer_id, c->mode_valid);
		drmModeFreeCrtc(c);
	}
	if (drmSetClientCap(k->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		error(k, "universal planes");
		return;
	}
	drmModePlaneRes *ps = drmModeGetPlaneResources(k->fd);
	if (!ps)
		return;
	for (uint32_t i = 0; i < ps->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(k->fd, ps->planes[i]);
		if (!p)
			continue;
		uint64_t type = 0;
		property(k, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
		fprintf(k->log, "plane=%u type=%llu crtc=%u possible_crtcs=0x%x\n", p->plane_id,
		        (unsigned long long)type, p->crtc_id, p->possible_crtcs);
		list_modifiers(k, p->plane_id);
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(ps);
}
static bool
save(struct comp_drm_kms *k, uint32_t obj, uint32_t type, const char *name)
{
	struct saved_prop *s = &k->props[k->count];
	s->object = obj;
	s->property = property(k, obj, type, name, &s->value);
	if (!s->property) {
		fprintf(k->log, "Missing atomic property %s on %u\n", name, obj);
		return false;
	}
	k->count++;
	return true;
}
static void
free_dumb(struct comp_drm_kms *k, struct dumb *d)
{
	if (d->map)
		munmap(d->map, d->size);
	if (d->fb)
		drmModeRmFB(k->fd, d->fb);
	if (d->handle) {
		struct drm_mode_destroy_dumb req = {.handle = d->handle};
		drmIoctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &req);
	}
	memset(d, 0, sizeof(*d));
}
static bool
alloc_dumb(struct comp_drm_kms *k, struct dumb *d)
{
	struct drm_mode_create_dumb req = {.width = k->mode.hdisplay, .height = k->mode.vdisplay, .bpp = 32};
	if (drmIoctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &req))
		return error(k, "CREATE_DUMB");
	d->handle = req.handle;
	d->pitch = req.pitch;
	d->size = req.size;
	uint32_t handles[4] = {d->handle}, pitches[4] = {d->pitch}, offsets[4] = {0};
	/* DUMB is explicitly linear; TEST_ONLY decides whether the chosen plane can scan it out. */
	if (drmModeAddFB2(k->fd, req.width, req.height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &d->fb, 0))
		return error(k, "AddFB2 XRGB8888 linear");
	struct drm_mode_map_dumb map = {.handle = d->handle};
	if (drmIoctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		return error(k, "MAP_DUMB");
	d->map = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, map.offset);
	if (d->map == MAP_FAILED) {
		d->map = NULL;
		return error(k, "mmap");
	}
	return true;
}
/* Order corresponds exactly to saved connector, CRTC, then plane properties. */
static drmModeAtomicReq *
request(struct comp_drm_kms *k, uint32_t fb, bool restore)
{
	drmModeAtomicReq *a = drmModeAtomicAlloc();
	if (!a)
		return NULL;
	uint64_t values[] = {k->crtc,
	                     k->blob,
	                     1,
	                     fb,
	                     k->crtc,
	                     0,
	                     0,
	                     (uint64_t)k->mode.hdisplay << 16,
	                     (uint64_t)k->mode.vdisplay << 16,
	                     0,
	                     0,
	                     k->mode.hdisplay,
	                     k->mode.vdisplay};
	for (unsigned i = 0; i < k->count; i++)
		if (drmModeAtomicAddProperty(a, k->props[i].object, k->props[i].property,
		                             restore ? k->props[i].value : values[i]) < 0) {
			drmModeAtomicFree(a);
			return NULL;
		}
	return a;
}
bool
comp_drm_kms_test(struct comp_drm_kms *k)
{
	drmModeAtomicReq *a = request(k, k->buffers[0].fb, false);
	if (!a)
		return false;
	int ret = drmModeAtomicCommit(k->fd, a, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(a);
	if (ret)
		return error(k, "atomic TEST_ONLY");
	fprintf(k->log, "atomic TEST_ONLY passed (no modeset applied)\n");
	return true;
}
bool
comp_drm_kms_prepare(struct comp_drm_kms *k, const char *connector, const char *mode, uint32_t requested_plane)
{
	if (!connector || !mode || k->count)
		return false;
	if (drmSetClientCap(k->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(k->fd, DRM_CLIENT_CAP_ATOMIC, 1))
		return error(k, "atomic client capability");
	if (!drmIsMaster(k->fd) && drmSetMaster(k->fd))
		return error(k, "DRM master (use an active local VT/seat ACL; do not loosen global permissions)");
	drmModeConnector *c = NULL;
	for (int i = 0; i < k->res->count_connectors; i++) {
		drmModeConnector *p = drmModeGetConnector(k->fd, k->res->connectors[i]);
		if (!p)
			continue;
		char name[64];
		snprintf(name, sizeof(name), "%s-%u", drmModeGetConnectorTypeName(p->connector_type),
		         p->connector_type_id);
		if (!strcmp(name, connector) && p->connection == DRM_MODE_CONNECTED) {
			c = p;
			break;
		}
		drmModeFreeConnector(p);
	}
	if (!c) {
		fprintf(k->log, "Connected connector '%s' not found\n", connector);
		return false;
	}
	bool found = false;
	for (int i = 0; i < c->count_modes; i++) {
		char name[80];
		snprintf(name, sizeof(name), "%ux%u@%.3f", c->modes[i].hdisplay, c->modes[i].vdisplay,
		         refresh(&c->modes[i]));
		if (!strcmp(name, mode)) {
			k->mode = c->modes[i];
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(k->log, "Mode '%s' not found; use exact enumeration including refresh\n", mode);
		drmModeFreeConnector(c);
		return false;
	}
	k->connector = c->connector_id;
	uint32_t possible = 0;
	for (int i = 0; i < c->count_encoders; i++) {
		drmModeEncoder *e = drmModeGetEncoder(k->fd, c->encoders[i]);
		if (e) {
			possible |= e->possible_crtcs;
			if (e->encoder_id == c->encoder_id)
				k->crtc = e->crtc_id;
		}
		drmModeFreeEncoder(e);
	}
	drmModeFreeConnector(c);
	int ci = -1;
	for (int i = 0; i < k->res->count_crtcs; i++)
		if (k->res->crtcs[i] == k->crtc && (possible & (1u << i)))
			ci = i;
	if (ci < 0)
		for (int i = 0; i < k->res->count_crtcs; i++)
			if (possible & (1u << i)) {
				drmModeCrtc *cr = drmModeGetCrtc(k->fd, k->res->crtcs[i]);
				bool unused = cr && !cr->mode_valid;
				drmModeFreeCrtc(cr);
				if (unused) {
					ci = i;
					k->crtc = k->res->crtcs[i];
					break;
				}
			}
	if (ci < 0) {
		fprintf(k->log, "No compatible unused/current CRTC\n");
		return false;
	}
	/* Refuse shared/mirrored CRTCs: restoring unrelated connectors is out of scope. */
	for (int i = 0; i < k->res->count_connectors; i++) {
		drmModeConnector *other = drmModeGetConnector(k->fd, k->res->connectors[i]);
		if (!other)
			continue;
		uint64_t cr = 0;
		property(k, other->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &cr);
		bool shared = other->connector_id != k->connector && cr == k->crtc;
		drmModeFreeConnector(other);
		if (shared) {
			fprintf(k->log, "Refusing shared CRTC\n");
			return false;
		}
	}
	if (drmModeCreatePropertyBlob(k->fd, &k->mode, sizeof(k->mode), &k->blob))
		return error(k, "mode blob");
	for (unsigned i = 0; i < 2; i++)
		if (!alloc_dumb(k, &k->buffers[i]))
			return false;
	if (!save(k, k->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID") ||
	    !save(k, k->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID") || !save(k, k->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"))
		return false;
	/* Keep our own reference to the previous mode blob throughout modesetting. */
	if (k->props[1].value) {
		drmModePropertyBlobRes *b = drmModeGetPropertyBlob(k->fd, k->props[1].value);
		uint32_t old = 0;
		if (!b || drmModeCreatePropertyBlob(k->fd, b->data, b->length, &old)) {
			drmModeFreePropertyBlob(b);
			return error(k, "save old mode blob");
		}
		drmModeFreePropertyBlob(b);
		k->props[1].value = old;
		k->old_blob = old;
	}
	drmModePlaneRes *ps = drmModeGetPlaneResources(k->fd);
	if (!ps)
		return false;
	const char *names[] = {"FB_ID", "CRTC_ID", "SRC_X",  "SRC_Y",  "SRC_W",
	                       "SRC_H", "CRTC_X",  "CRTC_Y", "CRTC_W", "CRTC_H"};
	/* Prefer primary, but permit an unused linear-capable overlay when primary is AFBC-only. */
	for (unsigned pass = 0; pass < 2 && !k->plane; pass++)
		for (uint32_t i = 0; i < ps->count_planes && !k->plane; i++) {
			drmModePlane *p = drmModeGetPlane(k->fd, ps->planes[i]);
			if (!p)
				continue;
			uint64_t type = 0;
			property(k, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
			bool eligible = (!p->crtc_id || p->crtc_id == k->crtc) && (p->possible_crtcs & (1u << ci)) &&
			                (!requested_plane || p->plane_id == requested_plane) &&
			                (pass == 0 ? type == DRM_PLANE_TYPE_PRIMARY
			                           : type == DRM_PLANE_TYPE_OVERLAY && !p->crtc_id);
			bool fmt = false;
			for (uint32_t j = 0; j < p->count_formats; j++)
				if (p->formats[j] == DRM_FORMAT_XRGB8888)
					fmt = true;
			uint32_t id = p->plane_id;
			drmModeFreePlane(p);
			if (!eligible || !fmt)
				continue;
			k->count = 3;
			bool valid = true;
			for (unsigned j = 0; j < 10; j++)
				if (!save(k, id, DRM_MODE_OBJECT_PLANE, names[j])) {
					valid = false;
					break;
				}
			if (valid && comp_drm_kms_test(k))
				k->plane = id;
		}
	drmModeFreePlaneResources(ps);
	if (!k->plane) {
		k->count = 3;
		fprintf(k->log, "No plane passed XRGB8888/LINEAR TEST_ONLY; no modeset performed\n");
		return false;
	}
	fprintf(k->log,
	        "Selected connector=%u crtc=%u plane=%u mode=%ux%u@%.3f format=XRGB8888 modifier=LINEAR pitch=%u; "
	        "CPU-copy diagnostic\n",
	        k->connector, k->crtc, k->plane, k->mode.hdisplay, k->mode.vdisplay, refresh(&k->mode),
	        k->buffers[0].pitch);
	return true;
}
static void
flip(int fd, unsigned sequence, unsigned sec, unsigned usec, void *data)
{
	(void)fd;
	(void)sequence;
	(void)sec;
	(void)usec;
	*(bool *)data = true;
}
static bool
present_fb(struct comp_drm_kms *k, uint32_t fb);
bool
comp_drm_kms_present(struct comp_drm_kms *k, const uint8_t *bgra, uint32_t stride)
{
	if (k->failed || !k->plane || stride < (uint32_t)k->mode.hdisplay * 4)
		return false;
	struct dumb *d = &k->buffers[k->next];
	for (unsigned y = 0; y < k->mode.vdisplay; y++)
		memcpy((uint8_t *)d->map + y * d->pitch, bgra + (size_t)y * stride, (size_t)k->mode.hdisplay * 4);
	bool result = present_fb(k, d->fb);
	if (result)
		k->scanout = -1;
	return result;
}
static bool
present_fb(struct comp_drm_kms *k, uint32_t fb)
{
	drmModeAtomicReq *a = request(k, fb, false);
	if (!a)
		return false;
	k->flip_done = false;
	int ret = drmModeAtomicCommit(
	    k->fd, a, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
	    &k->flip_done);
	drmModeAtomicFree(a);
	if (ret) {
		k->failed = true;
		return error(k, "atomic present");
	}
	k->changed = true;
	drmEventContext ev = {.version = 2, .page_flip_handler = flip};
	/* Timeout prevents indefinite waits on unplug/master loss. No buffer reuse after failure. */
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	while (!k->flip_done) {
		struct pollfd p = {.fd = k->fd, .events = POLLIN};
		ret = poll(&p, 1, 100);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0 || (ret > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
			k->failed = true;
			return error(k, "page flip poll");
		}
		if (ret > 0 && drmHandleEvent(k->fd, &ev)) {
			k->failed = true;
			return error(k, "page flip event");
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= 3) {
			fprintf(k->log, "Page flip timeout; refusing buffer reuse\n");
			k->failed = true;
			return false;
		}
	}
	k->next ^= 1;
	return true;
}
bool
comp_drm_kms_import(struct comp_drm_kms *k, unsigned index, int fd, uint32_t pitch, uint32_t offset)
{
	if (index >= 2 || k->imported[index].handle || k->changed || pitch < (uint32_t)k->mode.hdisplay * 4)
		return false;
	if (drmPrimeFDToHandle(k->fd, fd, &k->imported[index].handle))
		return error(k, "PRIME FD import");
	uint32_t handles[4] = {k->imported[index].handle}, pitches[4] = {pitch}, offsets[4] = {offset};
	uint64_t modifiers[4] = {DRM_FORMAT_MOD_LINEAR};
	int ret =
	    drmModeAddFB2WithModifiers(k->fd, k->mode.hdisplay, k->mode.vdisplay, DRM_FORMAT_XRGB8888, handles, pitches,
	                               offsets, modifiers, &k->imported[index].fb, DRM_MODE_FB_MODIFIERS);
	/* Some drivers accept implicit LINEAR but don't advertise it in IN_FORMATS.
	 * The Vulkan modifier is queried, never inferred; actual TEST_ONLY is mandatory. */
	if (ret && (errno == EINVAL || errno == EOPNOTSUPP))
		ret = drmModeAddFB2(k->fd, k->mode.hdisplay, k->mode.vdisplay, DRM_FORMAT_XRGB8888, handles, pitches,
		                    offsets, &k->imported[index].fb, 0);
	if (ret)
		return error(k, "DMA-BUF AddFB2 LINEAR");
	drmModeAtomicReq *a = request(k, k->imported[index].fb, false);
	if (!a)
		return false;
	ret = drmModeAtomicCommit(k->fd, a, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(a);
	if (ret)
		return error(k, "DMA-BUF atomic TEST_ONLY");
	fprintf(k->log, "DMA-BUF FB[%u]=%u handle=%u pitch=%u offset=%u XRGB8888/LINEAR TEST_ONLY passed\n", index,
	        k->imported[index].fb, k->imported[index].handle, pitch, offset);
	return true;
}
bool
comp_drm_kms_present_imported(struct comp_drm_kms *k, unsigned index)
{
	if (k->failed || !k->plane || index >= 2 || k->scanout == (int)index || !k->imported[index].fb)
		return false;
	uint32_t fb = k->imported[index].fb;
	if (!present_fb(k, fb))
		return false;
	k->scanout = (int)index;
	return true;
}
void
comp_drm_kms_close(struct comp_drm_kms *k)
{
	if (!k)
		return;
	if (k->changed) {
		drmModeAtomicReq *a = request(k, 0, true);
		if (!a || drmModeAtomicCommit(k->fd, a, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
			error(k, "RESTORE FAILED (check local VT display state)");
		else
			fprintf(k->log, "Previous atomic KMS state restored\n");
		drmModeAtomicFree(a);
	}
	for (unsigned i = 0; i < 2; i++) {
		if (k->imported[i].fb)
			drmModeRmFB(k->fd, k->imported[i].fb);
		if (k->imported[i].handle)
			drmCloseBufferHandle(k->fd, k->imported[i].handle);
		free_dumb(k, &k->buffers[i]);
	}
	if (k->old_blob)
		drmModeDestroyPropertyBlob(k->fd, k->old_blob);
	if (k->blob)
		drmModeDestroyPropertyBlob(k->fd, k->blob);
	drmModeFreeResources(k->res);
	close(k->fd);
	free(k);
}
uint32_t
comp_drm_kms_width(struct comp_drm_kms *k)
{
	return k->mode.hdisplay;
}
uint32_t
comp_drm_kms_height(struct comp_drm_kms *k)
{
	return k->mode.vdisplay;
}
uint64_t
comp_drm_kms_period_ns(struct comp_drm_kms *k)
{
	double hz = refresh(&k->mode);
	return hz > 0 ? (uint64_t)(1e9 / hz) : 16666667;
}
