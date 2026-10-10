// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define COMP_DRM_TIMING_SAMPLES 120
struct comp_drm_timing
{
	uint64_t last_ns, intervals[COMP_DRM_TIMING_SAMPLES], sum_ns, skipped, late;
	uint32_t last_sequence, count;
	bool have_last, sequence_valid;
};
/* Event timestamps are used only as differences: no assumed clock relationship
 * to Vulkan/CPU clocks. A pause/restart discards the incomplete window. */
static inline bool
comp_drm_timing_add(struct comp_drm_timing *s, uint64_t ns, uint32_t sequence)
{
	if (!s->have_last || ns <= s->last_ns || ns - s->last_ns > UINT64_C(1000000000)) {
		memset(s, 0, sizeof(*s));
		s->last_ns = ns;
		s->last_sequence = sequence;
		s->have_last = s->sequence_valid = true;
		return false;
	}
	uint64_t interval = ns - s->last_ns;
	uint32_t delta = sequence - s->last_sequence; /* Handles 32-bit sequence wrap. */
	s->last_ns = ns;
	s->last_sequence = sequence;
	if (!delta || delta > 10000)
		s->sequence_valid = false;
	else if (delta > 1) {
		s->skipped += delta - 1;
		s->late++;
	}
	s->intervals[s->count++] = interval;
	s->sum_ns += interval;
	return s->count == COMP_DRM_TIMING_SAMPLES;
}
static inline int
comp_drm_timing_compare(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}
static inline void
comp_drm_timing_reset_window(struct comp_drm_timing *s)
{
	s->count = 0;
	s->sum_ns = s->skipped = s->late = 0;
	s->sequence_valid = true;
}
