// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
extern "C" {
#include "comp_drm_timing.h"
#include "comp_drm_pipeline.h"
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

TEST_CASE("KMS triple buffering never renders scanout or queued slots", "[drm][pipeline]")
{
	comp_drm_slots s = {-1, -1};
	REQUIRE(comp_drm_slot_free(&s, 0));
	s.queued = 0;
	REQUIRE_FALSE(comp_drm_slot_free(&s, 0));
	REQUIRE(comp_drm_slot_free(&s, 1));
	s.scanout = 0;
	s.queued = 1;
	REQUIRE_FALSE(comp_drm_slot_free(&s, 0));
	REQUIRE_FALSE(comp_drm_slot_free(&s, 1));
	REQUIRE(comp_drm_slot_free(&s, 2));
	// Flip retires old scanout 0; slot 2 can be submitted next.
	s.scanout = 1;
	s.queued = 2;
	REQUIRE(comp_drm_slot_free(&s, 0));
	REQUIRE_FALSE(comp_drm_slot_free(&s, 1));
	REQUIRE_FALSE(comp_drm_slot_free(&s, 2));
	REQUIRE_FALSE(comp_drm_slot_free(&s, 3));
}
TEST_CASE("KMS predictions use measured budget and vblank phase", "[drm][pipeline]")
{
	const int64_t phase = 1000000000, period = 11111000;
	REQUIRE(comp_drm_predict_present(phase + 1000000, phase, period, 9000000) == phase + period);
	REQUIRE(comp_drm_predict_present(phase + 1000000, phase, period, 12000000) == phase + 2 * period);
	REQUIRE(comp_drm_predict_present(phase + 5 * period, phase, period, 1000000) == phase + 6 * period);
	REQUIRE(comp_drm_predict_present(phase, 0, period, period) == phase + period);
}
