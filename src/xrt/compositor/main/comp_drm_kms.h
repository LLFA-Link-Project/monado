// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct comp_drm_kms;
/* Commit/flip observations run on the caller's event-pump thread. A flip_receipt
 * is NOT a kernel presentation timestamp when the event clock is unmapped. */
typedef void (*comp_drm_kms_observer)(void *data, const char *event, int index, uint64_t ns, uint64_t value);
void
comp_drm_kms_set_observer(struct comp_drm_kms *k, comp_drm_kms_observer observer, void *data);
/* No display change in open, list, prepare, or test. Connector and mode must
 * be explicitly selected for anything beyond enumeration. */
struct comp_drm_kms *
comp_drm_kms_open(const char *device, FILE *log);
void
comp_drm_kms_list(struct comp_drm_kms *k);
bool
comp_drm_kms_prepare(struct comp_drm_kms *k, const char *connector, const char *mode, uint32_t plane);
bool
comp_drm_kms_test(struct comp_drm_kms *k);
bool
comp_drm_kms_present(struct comp_drm_kms *k, const uint8_t *bgra, uint32_t stride);
void
comp_drm_kms_close(struct comp_drm_kms *k);
uint32_t
comp_drm_kms_width(struct comp_drm_kms *k);
uint32_t
comp_drm_kms_height(struct comp_drm_kms *k);
uint64_t
comp_drm_kms_period_ns(struct comp_drm_kms *k);

/* Import one single-plane LINEAR BGRA allocation. The caller retains the FD;
 * KMS owns its GEM/FB references until close. TEST_ONLY precedes every import's use. */
bool
comp_drm_kms_import(struct comp_drm_kms *k, unsigned index, int fd, uint32_t pitch, uint32_t offset);
bool
comp_drm_kms_present_imported(struct comp_drm_kms *k, unsigned index);

/* At most one queued atomic commit; nonblocking pump returns true while pending.
 * Only free slots may be rendered. Synchronous legacy probe API is retained. */
bool
comp_drm_kms_pump(struct comp_drm_kms *k, bool wait);
bool
comp_drm_kms_slot_free(struct comp_drm_kms *k, unsigned index);
bool
comp_drm_kms_queue_imported(struct comp_drm_kms *k, unsigned index);
uint64_t
comp_drm_kms_last_flip_ns(struct comp_drm_kms *k);

bool
comp_drm_kms_has_in_fence(struct comp_drm_kms *k);
/* Consumes sync_fd on ALL paths. -1 means GPU work is already complete.
 * Atomic commit keeps its own kernel reference until the input fence signals. */
bool
comp_drm_kms_queue_imported_fenced(struct comp_drm_kms *k, unsigned index, int sync_fd);

/* After importing this image: IN_FENCE_FD or verified Rockchip reservation
 * sync-file ioctl. Other drivers require IN_FENCE_FD. */
bool
comp_drm_kms_supports_sync_fd(struct comp_drm_kms *k, unsigned index);
