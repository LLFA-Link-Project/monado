// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#include "arpara_optics.h"
#include "arpara_optics_generated.h"
#include "util/u_logging.h"
#include <cjson/cJSON.h>
#include <cmath>
#include <cstring>
#include <limits>

static bool
number(const cJSON *value, float *out)
{
	if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
	    std::abs(value->valuedouble) > std::numeric_limits<float>::max())
		return false;
	*out = (float)value->valuedouble;
	return true;
}
static bool
field(const cJSON *object, const char *key, float *out, bool optional = false)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
	return value ? number(value, out) : optional;
}
static bool
string_is(const cJSON *object, const char *key, const char *expected)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsString(value) && strcmp(value->valuestring, expected) == 0;
}
static bool
channel(const cJSON *object, u_vive_values *eye, unsigned color)
{
	if (!cJSON_IsObject(object) || !string_is(object, "type", "DISTORT_POLY3") ||
	    !field(object, "center_x", &eye->center[color].x, true) ||
	    !field(object, "center_y", &eye->center[color].y, true))
		return false;
	const cJSON *coeffs = cJSON_GetObjectItemCaseSensitive(object, "coeffs");
	int size = cJSON_GetArraySize(coeffs);
	if (!cJSON_IsArray(coeffs) || (size != 3 && size != 8))
		return false;
	for (int i = 0; i < size; ++i) {
		float value;
		if (!number(cJSON_GetArrayItem(coeffs, i), &value) || (i >= 3 && value != 0))
			return false;
		if (i < 3)
			eye->coefficients[color][i] = value;
	}
	return true;
}
static bool
parse(const cJSON *root, arpara_optics *out)
{
	const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
	float width, height, windows;
	if (!cJSON_IsObject(root) || !cJSON_IsObject(device) || !string_is(device, "first_eye", "eEYE_LEFT") ||
	    !string_is(device, "last_eye", "eEYE_RIGHT") || !field(device, "eye_target_width_in_pixels", &width) ||
	    !field(device, "eye_target_height_in_pixels", &height) || width < 1 || height < 1 || width > 65536 ||
	    height > 65536 || floorf(width) != width || floorf(height) != height ||
	    !field(device, "num_windows", &windows) || windows != 1)
		return false;
	out->recommended_width = (uint32_t)width;
	out->recommended_height = (uint32_t)height;
	float aspect = width / height;
	if (!field(device, "physical_aspect_x_over_y", &aspect, true) || aspect <= 0)
		return false;
	const cJSON *eyes = cJSON_GetObjectItemCaseSensitive(root, "tracking_to_eye_transform");
	if (!cJSON_IsArray(eyes) || cJSON_GetArraySize(eyes) != 2)
		return false;
	for (unsigned i = 0; i < 2; ++i) {
		u_vive_values &eye = out->eyes[i];
		eye.aspect_x_over_y = aspect;
		const cJSON *object = cJSON_GetArrayItem(eyes, i);
		if (!field(object, "grow_for_undistort", &eye.grow_for_undistort) || eye.grow_for_undistort < 0 ||
		    !field(object, "undistort_r2_cutoff", &eye.undistort_r2_cutoff) || eye.undistort_r2_cutoff <= 0)
			return false;
		const cJSON *matrix = cJSON_GetObjectItemCaseSensitive(object, "intrinsics");
		float m[9];
		if (!cJSON_IsArray(matrix) || cJSON_GetArraySize(matrix) != 3)
			return false;
		for (int row = 0; row < 3; ++row) {
			const cJSON *values = cJSON_GetArrayItem(matrix, row);
			if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) != 3)
				return false;
			for (int col = 0; col < 3; ++col)
				if (!number(cJSON_GetArrayItem(values, col), &m[row * 3 + col]))
					return false;
		}
		if (m[0] <= 0 || m[4] <= 0 || m[1] != 0 || m[3] != 0 || m[6] != 0 || m[7] != 0 ||
		    (m[8] != 1 && m[8] != -1))
			return false;
		eye.intrinsic_focus = {m[0], m[4]};
		eye.intrinsic_center = {m[2], m[5]};
		const cJSON *green = cJSON_GetObjectItemCaseSensitive(object, "distortion");
		const char *names[] = {"distortion_red", "distortion", "distortion_blue"};
		for (unsigned c = 0; c < 3; ++c) {
			const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, names[c]);
			if (!channel(value ? value : green, &eye, c))
				return false;
		}
		float scale = 1 + eye.grow_for_undistort;
		if (!std::isfinite(scale) || fabsf(m[2]) >= scale || fabsf(m[5]) >= scale)
			return false;
		out->fov[i] = {-atanf((scale - m[2]) / m[0]), atanf((scale + m[2]) / m[0]),
		               atanf((scale + m[5]) / m[4]), -atanf((scale - m[5]) / m[4])};
	}
	// Reject divergent calibrations before any mesh or HID streaming is started.
	for (unsigned eye = 0; eye < 2; ++eye)
		for (unsigned y = 0; y <= 32; ++y)
			for (unsigned x = 0; x <= 32; ++x) {
				xrt_uv_triplet uv;
				if (!arpara_optics_compute(out, eye, x / 32.f, y / 32.f, &uv))
					return false;
			}
	return true;
}

bool
arpara_optics_parse(const char *json, arpara_optics *out)
{
	if (!json || !out)
		return false;
	cJSON *root = cJSON_ParseWithOpts(json, nullptr, true);
	arpara_optics result{};
	bool ok = root && parse(root, &result);
	cJSON_Delete(root);
	if (ok)
		*out = result;
	else
		U_LOG_E("arpara: invalid/unsupported optical calibration (requires two DISTORT_POLY3 eyes)");
	return ok;
}

bool
arpara_optics_compute(const arpara_optics *optics, uint32_t eye, float u, float v, xrt_uv_triplet *out)
{
	if (!optics || !out || eye >= 2 || !std::isfinite(u) || !std::isfinite(v))
		return false;
	const u_vive_values &val = optics->eyes[eye];
	xrt_vec2 channels[3];
	for (unsigned c = 0; c < 3; ++c) {
		float x = 2 * u - 1 - val.center[c].x;
		float y = (2 * v - 1) / val.aspect_x_over_y - val.center[c].y;
		float r2 = x * x + y * y;
		const float *k = val.coefficients[c];
		// Valve DISTORT_POLY3: polynomial, NOT the reciprocal used by Vive's DPOLY3.
		float d = 1 + r2 * (k[0] + r2 * (k[1] + r2 * k[2]));
		float factor = 0.5f / (1 + val.grow_for_undistort);
		channels[c] = {0.5f + (x * d + val.center[c].x) * factor,
		               0.5f + (y * d + val.center[c].y) * factor * val.aspect_x_over_y};
		if (d <= 0 || !std::isfinite(channels[c].x) || !std::isfinite(channels[c].y))
			return false;
	}
	out->r = channels[0];
	out->g = channels[1];
	out->b = channels[2];
	return true;
}

bool
arpara_optics_load_builtin(arpara_optics *out)
{
	return arpara_optics_parse(arpara_optics_json, out);
}
const char *
arpara_optics_source_hash()
{
	return arpara_optics_source_sha256;
}
