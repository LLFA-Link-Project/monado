// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
extern "C" {
#include "comp_drm_timing.h"
}
TEST_CASE("KMS timing counts real event intervals and sequence gaps", "[drm][timing]")
{
	comp_drm_timing s = {};
	uint64_t ns = 1000000000;
	uint32_t sequence = UINT32_MAX - 60;
	REQUIRE_FALSE(comp_drm_timing_add(&s, ns, sequence));
	for (unsigned i = 0; i < 120; i++) {
		unsigned delta = i == 40 ? 2 : 1;
		ns += delta * UINT64_C(11111000);
		sequence += delta;
		REQUIRE(comp_drm_timing_add(&s, ns, sequence) == (i == 119));
	}
	REQUIRE(s.count == 120);
	REQUIRE(s.skipped == 1);
	REQUIRE(s.late == 1);
	REQUIRE(s.sequence_valid);
	REQUIRE(s.sum_ns == 121 * UINT64_C(11111000));
	qsort(s.intervals, s.count, sizeof(s.intervals[0]), comp_drm_timing_compare);
	REQUIRE(s.intervals[113] == 11111000);
	REQUIRE(s.intervals[119] == 22222000);
	comp_drm_timing_reset_window(&s);
	REQUIRE(s.count == 0);
	REQUIRE_FALSE(comp_drm_timing_add(&s, ns + 11111000, sequence + 1));
	REQUIRE(s.count == 1);
}
TEST_CASE("KMS timing excludes pause boundaries and marks absent sequence data", "[drm][timing]")
{
	comp_drm_timing s = {};
	comp_drm_timing_add(&s, 1000000000, 10);
	comp_drm_timing_add(&s, 1010000000, 10);
	REQUIRE_FALSE(s.sequence_valid);
	comp_drm_timing_add(&s, 3010000000, 200);
	REQUIRE(s.count == 0);
	REQUIRE(s.skipped == 0);
	REQUIRE(s.sequence_valid);
	comp_drm_timing_add(&s, 3020000000, 201);
	REQUIRE(s.count == 1);
	comp_drm_timing_add(&s, 1, 1);
	REQUIRE(s.count == 0);
}
