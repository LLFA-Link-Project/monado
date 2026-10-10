// Copyright 2026, OpenAI.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
extern "C" int
drm_kms_fence_fixture(unsigned scenario);
TEST_CASE("KMS sync FD handoff, implicit reservation and FD cleanup", "[drm][fence]")
{
	for (unsigned scenario = 0; scenario < 10; scenario++) {
		INFO("scenario=" << scenario);
		REQUIRE(drm_kms_fence_fixture(scenario) == 0);
	}
}
