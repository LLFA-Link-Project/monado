// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#include "target_llfa.h"
#include "multi_wrapper/multi.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

bool
t_llfa_parse_mount(const char *text, struct xrt_pose *mount)
{
	struct xrt_pose p = XRT_POSE_IDENTITY;
	if (text != NULL && text[0] != '\0') {
		char trailing;
		if (sscanf(text, "%f,%f,%f,%f,%f,%f,%f %c", &p.position.x, &p.position.y, &p.position.z,
		           &p.orientation.x, &p.orientation.y, &p.orientation.z, &p.orientation.w, &trailing) != 7) {
			return false;
		}
	}
	float values[] = {p.position.x,    p.position.y,    p.position.z,   p.orientation.x,
	                  p.orientation.y, p.orientation.z, p.orientation.w};
	for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		if (!isfinite(values[i])) {
			return false;
		}
	}
	double norm = sqrt((double)p.orientation.x * p.orientation.x + (double)p.orientation.y * p.orientation.y +
	                   (double)p.orientation.z * p.orientation.z + (double)p.orientation.w * p.orientation.w);
	if (norm < 1e-6 || !isfinite(norm)) {
		return false;
	}
	p.orientation.x /= norm;
	p.orientation.y /= norm;
	p.orientation.z /= norm;
	p.orientation.w /= norm;
	*mount = p;
	return true;
}

static bool
has_tracker_pose(struct xrt_device *d)
{
	for (uint32_t i = 0; i < d->input_count; i++) {
		if (d->inputs[i].name == XRT_INPUT_GENERIC_TRACKER_POSE) {
			return true;
		}
	}
	return false;
}

int
t_llfa_select_tracker(struct xrt_device **devices, uint32_t count, const char *serial)
{
	int selected = -1;
	bool explicit_serial = serial != NULL && serial[0] != '\0';
	for (uint32_t i = 0; i < count; i++) {
		struct xrt_device *d = devices[i];
		if (d == NULL ||
		    (explicit_serial ? strcmp(d->serial, serial) != 0 : d->name != XRT_DEVICE_VIVE_TRACKER_TUNDRA)) {
			continue;
		}
		if (d->device_type != XRT_DEVICE_TYPE_GENERIC_TRACKER || !has_tracker_pose(d) ||
		    !d->supported.position_tracking || !d->supported.orientation_tracking || selected >= 0) {
			return -2;
		}
		selected = (int)i;
	}
	return selected;
}

xrt_result_t
t_llfa_wrap_head(struct xrt_system_devices *system,
                 uint32_t head_index,
                 uint32_t tracker_index,
                 const struct xrt_pose *mount)
{
	if (head_index >= system->static_xdev_count || tracker_index >= system->static_xdev_count ||
	    head_index == tracker_index || system->static_xdevs[head_index] == NULL ||
	    system->static_xdevs[tracker_index] == NULL ||
	    system->static_xdevs[head_index]->device_type != XRT_DEVICE_TYPE_HMD ||
	    t_llfa_select_tracker(&system->static_xdevs[tracker_index], 1,
	                          system->static_xdevs[tracker_index]->serial) != 0) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	struct xrt_pose offset = *mount;
	struct xrt_device *wrapper = multi_create_tracking_override(
	    XRT_TRACKING_OVERRIDE_DIRECT, system->static_xdevs[head_index], system->static_xdevs[tracker_index],
	    XRT_INPUT_GENERIC_TRACKER_POSE, &offset);
	if (wrapper == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	system->static_xdevs[head_index] = wrapper;
	return XRT_SUCCESS;
}
