// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "util/u_distortion_mesh.h"
#ifdef __cplusplus
extern "C" {
#endif
struct arpara_optics
{
	struct u_vive_values eyes[2];
	struct xrt_fov fov[2];
	uint32_t recommended_width, recommended_height;
};
bool
arpara_optics_parse(const char *json, struct arpara_optics *out);
bool
arpara_optics_compute(const struct arpara_optics *optics, uint32_t eye, float u, float v, struct xrt_uv_triplet *out);
bool
arpara_optics_load_builtin(struct arpara_optics *out);
const char *
arpara_optics_source_hash(void);
#ifdef __cplusplus
}
#endif
