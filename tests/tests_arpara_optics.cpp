// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "arpara/arpara_optics.h"
#include "util/u_json.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <limits>

static std::string
fixture()
{
	std::ifstream stream(ARPARA_OPTICS_FIXTURE);
	std::ostringstream text;
	text << stream.rdbuf();
	return text.str();
}
TEST_CASE("Official arpara optics use Valve POLY3 instead of Vive reciprocal", "[arpara][optics]")
{
	arpara_optics optics{};
	REQUIRE(arpara_optics_parse(fixture().c_str(), &optics));
	REQUIRE(optics.recommended_width == 2544);
	REQUIRE(optics.recommended_height == 2544);
	for (unsigned eye = 0; eye < 2; ++eye) {
		const auto &val = optics.eyes[eye];
		CHECK(val.aspect_x_over_y == 1);
		CHECK(val.intrinsic_focus.x == Catch::Approx(1.33599997));
		CHECK(val.grow_for_undistort == Catch::Approx(0.600000024));
		CHECK(val.undistort_r2_cutoff == Catch::Approx(1.39999998));
		CHECK(optics.fov[eye].angle_right == Catch::Approx(atan(1.600000024 / 1.33599997)));
		CHECK(optics.fov[eye].angle_left == -optics.fov[eye].angle_right);
		for (unsigned color = 0; color < 3; ++color) {
			CHECK(val.coefficients[color][0] == Catch::Approx(0.172767789));
			CHECK(val.coefficients[color][1] == Catch::Approx(0.015039393));
			CHECK(val.coefficients[color][2] == Catch::Approx(0.055597465));
			CHECK(val.center[color].x == 0);
			CHECK(val.center[color].y == 0);
		}
		// Independent double-precision evaluation of Valve's published equation.
		for (unsigned y = 0; y <= 16; ++y)
			for (unsigned x = 0; x <= 16; ++x) {
				double u = x / 16., v = y / 16.;
				double px = 2 * u - 1, py = 2 * v - 1;
				double r2 = px * px + py * py;
				double d = 1 + .172767789 * r2 + .015039393 * r2 * r2 + .055597465 * r2 * r2 * r2;
				xrt_uv_triplet result{};
				REQUIRE(arpara_optics_compute(&optics, eye, (float)u, (float)v, &result));
				for (auto channel : {result.r, result.g, result.b}) {
					CHECK(std::isfinite(channel.x));
					CHECK(std::isfinite(channel.y));
					CHECK(channel.x == Catch::Approx(.5 + px * d / (2 * 1.600000024)).margin(1e-6));
					CHECK(channel.y == Catch::Approx(.5 + py * d / (2 * 1.600000024)).margin(1e-6));
				}
			}
	}
	xrt_uv_triplet out{};
	CHECK_FALSE(arpara_optics_compute(&optics, 2, .5f, .5f, &out));
	CHECK_FALSE(arpara_optics_compute(&optics, 0, std::numeric_limits<float>::quiet_NaN(), .5f, &out));
}

TEST_CASE("Optics parser preserves independent eyes and optional RGB models", "[arpara][optics]")
{
	cJSON *root = cJSON_Parse(fixture().c_str());
	REQUIRE(root != nullptr);
	cJSON *eyes = cJSON_GetObjectItemCaseSensitive(root, "tracking_to_eye_transform");
	cJSON *left = cJSON_GetArrayItem(eyes, 0), *right = cJSON_GetArrayItem(eyes, 1);
	cJSON *red = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(left, "distortion"), true);
	cJSON_ReplaceItemInArray(cJSON_GetObjectItemCaseSensitive(red, "coeffs"), 0, cJSON_CreateNumber(.3));
	cJSON_AddNumberToObject(red, "center_x", .1);
	cJSON_AddItemToObject(left, "distortion_red", red);
	cJSON *blue = cJSON_Duplicate(red, true);
	cJSON_ReplaceItemInArray(cJSON_GetObjectItemCaseSensitive(blue, "coeffs"), 0, cJSON_CreateNumber(.05));
	cJSON_AddItemToObject(left, "distortion_blue", blue);
	cJSON_ReplaceItemInObjectCaseSensitive(right, "grow_for_undistort", cJSON_CreateNumber(.8));
	char *text = cJSON_PrintUnformatted(root);
	arpara_optics optics{};
	REQUIRE(arpara_optics_parse(text, &optics));
	xrt_uv_triplet l{}, r{};
	REQUIRE(arpara_optics_compute(&optics, 0, .9f, .8f, &l));
	REQUIRE(arpara_optics_compute(&optics, 1, .9f, .8f, &r));
	CHECK(l.r.x != l.g.x);
	CHECK(l.b.x != l.g.x);
	CHECK(l.r.x != l.b.x);
	CHECK(l.g.x != r.g.x);
	cJSON_free(text);
	cJSON_Delete(root);
}

TEST_CASE("Invalid optical structures and unsupported models fail without fallback", "[arpara][optics]")
{
	arpara_optics optics{};
	CHECK_FALSE(arpara_optics_parse("{", &optics));
	CHECK_FALSE(arpara_optics_parse("{}", &optics));
	CHECK_FALSE(arpara_optics_parse((fixture() + "junk").c_str(), &optics));
	for (const char *key : {"intrinsics", "distortion", "grow_for_undistort", "undistort_r2_cutoff"}) {
		cJSON *root = cJSON_Parse(fixture().c_str());
		cJSON *eye = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "tracking_to_eye_transform"), 0);
		cJSON_DeleteItemFromObjectCaseSensitive(eye, key);
		char *text = cJSON_PrintUnformatted(root);
		CHECK_FALSE(arpara_optics_parse(text, &optics));
		cJSON_free(text);
		cJSON_Delete(root);
	}
	std::string wrong = fixture();
	wrong.replace(wrong.find("DISTORT_POLY3"), strlen("DISTORT_POLY3"), "DISTORT_DPOLY3");
	CHECK_FALSE(arpara_optics_parse(wrong.c_str(), &optics));
	for (const char *key : {"num_windows", "eye_target_width_in_pixels"}) {
		cJSON *root = cJSON_Parse(fixture().c_str());
		cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
		cJSON_ReplaceItemInObjectCaseSensitive(device, key, cJSON_CreateNumber(0));
		char *text = cJSON_PrintUnformatted(root);
		CHECK_FALSE(arpara_optics_parse(text, &optics));
		cJSON_free(text);
		cJSON_Delete(root);
	}
}
