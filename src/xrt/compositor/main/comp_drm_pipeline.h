// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define COMP_DRM_IMAGES 3
struct comp_drm_slots
{
	int scanout, queued;
};
static inline bool
comp_drm_slot_free(const struct comp_drm_slots *s, unsigned i)
{
	return i < COMP_DRM_IMAGES && (int)i != s->scanout && (int)i != s->queued;
}
/* Earliest vblank leaving enough CPU/queue work budget. Arithmetic stays in
 * monotonic ns; the caller validates the KMS timestamp clock. */
static inline int64_t
comp_drm_predict_present(int64_t now, int64_t phase, int64_t period, int64_t budget)
{
	int64_t deadline = now + budget;
	if (!phase || phase > now)
		phase = now;
	int64_t steps = (deadline - phase + period - 1) / period;
	return phase + steps * period;
}
