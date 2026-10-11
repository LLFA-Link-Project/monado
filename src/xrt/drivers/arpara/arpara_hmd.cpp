// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#include "arpara_interface.h"
#include "arpara_protocol.h"
#include "arpara_optics.h"
#include "math/m_imu_3dof.h"
#include "os/os_hid.h"
#include "os/os_threading.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_visibility_mask.h"
#include <stdio.h>
#include <stddef.h>

static_assert(sizeof(arpara_rs_decoded) == 112, "Rust/C++ report ABI size mismatch");
static_assert(alignof(arpara_rs_decoded) == 4, "Rust/C++ report ABI alignment mismatch");
static_assert(offsetof(arpara_rs_decoded, counter) == 40, "Rust/C++ counter ABI mismatch");
static_assert(offsetof(arpara_rs_decoded, ack) == 45, "Rust/C++ ACK ABI mismatch");

DEBUG_GET_ONCE_BOOL_OPTION(arpara_imu, "ARPARA_IMU", true)
DEBUG_GET_ONCE_BOOL_OPTION(arpara_pose_timing, "ARPARA_POSE_TIMING", false)

struct arpara_hmd
{
	struct xrt_device base;
	struct arpara_optics optics;
	struct os_hid_device *hid;
	struct os_thread_helper thread;
	struct os_mutex mutex;
	struct m_imu_3dof fusion;
	struct xrt_quat orientation;
	uint64_t last_host_ns;
	uint64_t fusion_ns;
	uint64_t sample_fusion_ns;
	uint64_t samples;
	uint64_t first_host_ns;
	bool orientation_ready;
	uint16_t counter;
	bool have_counter;
	bool disconnected;
};

static bool
send_report(struct arpara_hmd *h, const uint8_t report[65])
{
	int ret = os_hid_write(h->hid, report, 65);
	if (ret != 65) {
		U_LOG_E("arpara: HID write CMD 0x%02x failed/short: %d", report[3], ret);
		return false;
	}
	return true;
}

/* Isolated provisional identity axis transform: physical signs and mounting
 * are unverified. accel is converted from g to m/s^2, gyro from deg/s to rad/s.
 * Magnetometer is validated but not used for yaw correction without calibration. */
static void
sensor_vectors(const struct arpara_rs_decoded *s, struct xrt_vec3 *accel, struct xrt_vec3 *gyro)
{
	*accel = xrt_vec3{s->values[0] * 9.80665f, s->values[1] * 9.80665f, s->values[2] * 9.80665f};
	const float radians = 0.017453292519943295f;
	*gyro = xrt_vec3{s->values[3] * radians, s->values[4] * radians, s->values[5] * radians};
}

/* Called only by the HID reader, including during startup response waits. */
static bool
handle_report(struct arpara_hmd *h, const uint8_t *p, size_t n)
{
	struct arpara_rs_decoded s;
	if (!arpara_rs_decode(p, n, &s)) {
		return true; // Rust rejected corrupt/short/nonfinite reports.
	}
	if (s.kind == 2 && s.ack_requested) {
		bool ok = send_report(h, s.ack);
		if (ok) {
			U_LOG_D("arpara: acknowledged status request");
		}
		return ok;
	}
	if (s.kind != 1) {
		return true;
	}
	uint64_t now = os_monotonic_get_ns();
	bool ready = false;
	uint64_t delta = h->have_counter ? arpara_rs_counter_delta_ns(s.counter, h->counter) : 0;
	if (h->have_counter && s.counter == h->counter) {
		return true; // Duplicate timestamp.
	}
	/* Device ticks drive fusion intervals; freshness uses host receipt time.
	 * A stall/wrap ambiguity reseeds timing without integrating a large gap. */
	if (!h->have_counter || delta == 0 || now - h->last_host_ns > 100000000) {
		h->fusion.state = M_IMU_3DOF_STATE_START;
		h->fusion_ns = now > h->fusion_ns ? now : h->fusion_ns + 1;
	} else {
		h->fusion_ns += delta;
		ready = true;
	}
	h->counter = s.counter;
	h->have_counter = true;
	struct xrt_vec3 accel, gyro;
	sensor_vectors(&s, &accel, &gyro);
	m_imu_3dof_update(&h->fusion, h->fusion_ns, &accel, &gyro);
	os_mutex_lock(&h->mutex);
	h->orientation = h->fusion.rot;
	h->last_host_ns = now;
	h->sample_fusion_ns = h->fusion_ns;
	h->orientation_ready = ready;
	if (h->samples == 0) {
		h->first_host_ns = now;
	}
	h->samples++;
	os_mutex_unlock(&h->mutex);
	if (h->samples == 1001 && now > h->first_host_ns) {
		U_LOG_I("arpara: observed first 1000 IMU intervals: %.1f Hz (host receipt timing)",
		        1000.0 * 1e9 / (double)(now - h->first_host_ns));
	}
	if (h->samples == 2) {
		U_LOG_I("arpara: IMU orientation updates started (experimental axes, 3DoF only)");
	}
	return true;
}

static int
read_report(struct arpara_hmd *h, uint8_t report[64])
{
	int ret = os_hid_read(h->hid, report, 64, 50);
	if (ret < 0) {
		U_LOG_E("arpara: HID read failed/device disconnected: %d", ret);
		os_mutex_lock(&h->mutex);
		h->disconnected = true;
		os_mutex_unlock(&h->mutex);
	}
	return ret;
}

static bool
initialize(struct arpara_hmd *h)
{
	size_t count = arpara_rs_startup(0, NULL, NULL);
	for (size_t i = 0; i < count; i++) {
		if (!os_thread_helper_is_running(&h->thread)) {
			return false;
		}
		uint8_t request[65];
		uint32_t timeout_ms;
		arpara_rs_startup(i, request, &timeout_ms);
		uint8_t command = request[3];
		U_LOG_I("arpara: startup %zu/%zu CMD 0x%02x (Rust)", i + 1, count, command);
		if (!send_report(h, request)) {
			return false;
		}
		uint64_t deadline = os_monotonic_get_ns() + (uint64_t)timeout_ms * 1000000;
		bool received = false;
		while (os_thread_helper_is_running(&h->thread) && (uint64_t)os_monotonic_get_ns() < deadline) {
			uint8_t report[64];
			int n = read_report(h, report);
			if (n < 0 || (n > 0 && !handle_report(h, report, (size_t)n))) {
				return false;
			}
			struct arpara_rs_decoded decoded;
			if (n <= 0 || !arpara_rs_decode(report, (size_t)n, &decoded) || decoded.kind != 3 ||
			    decoded.command != command) {
				continue;
			}
			U_LOG_I("arpara: CMD 0x%02x response status 0x%02x", command, decoded.status);
			if (decoded.status != 0) {
				return false;
			}
			received = true;
			break;
		}
		if (!received) {
			U_LOG_W("arpara: CMD 0x%02x response timeout or shutdown", command);
			return false;
		}
	}
	U_LOG_I("arpara: initialization complete: accel/gyro 1000 Hz, magnetometer 200 Hz requested and accepted");
	return true;
}

static void *
reader(void *ptr)
{
	struct arpara_hmd *h = (struct arpara_hmd *)ptr;
	os_thread_name_self("arpara HID");
	if (!initialize(h)) {
		U_LOG_W(
		    "arpara: initialization incomplete; HMD remains registered, tracking requires fresh IMU samples");
	}
	while (os_thread_helper_is_running(&h->thread)) {
		os_mutex_lock(&h->mutex);
		bool disconnected = h->disconnected;
		os_mutex_unlock(&h->mutex);
		if (disconnected) {
			break;
		}
		uint8_t report[64];
		int n = read_report(h, report);
		if (n < 0 || (n > 0 && !handle_report(h, report, (size_t)n))) {
			break;
		}
	}
	return NULL;
}

static xrt_result_t
get_pose(struct xrt_device *xdev, enum xrt_input_name name, int64_t at_timestamp_ns, struct xrt_space_relation *out)
{
	struct arpara_hmd *h = (struct arpara_hmd *)xdev;
	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	/* Latest sample only: no speculative prediction with unverified calibration. */
	*out = {};
	os_mutex_lock(&h->mutex);
	uint64_t sample_ns = h->last_host_ns;
	uint64_t device_time_ns = h->sample_fusion_ns;
	int64_t query_ns = os_monotonic_get_ns();
	out->pose.orientation = h->orientation;
	/* 3DoF uses a valid fixed origin, without claiming position tracking.
	 * OpenXR clients need both valid components to locate VIEW in STAGE. */
	out->relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                      XRT_SPACE_RELATION_POSITION_VALID_BIT);
	if (!h->disconnected && h->orientation_ready && os_monotonic_get_ns() - h->last_host_ns < 100000000) {
		out->relation_flags =
		    (enum xrt_space_relation_flags)(out->relation_flags | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	}
	os_mutex_unlock(&h->mutex);
	if (debug_get_bool_option_arpara_pose_timing()) {
		U_LOG_I(
		    "arpara pose timing: requested_ns=%lld receipt_ns=%llu query_ns=%lld fusion_ns=%llu "
		    "orientation=[%.6f,%.6f,%.6f,%.6f]; prediction=none",
		    (long long)at_timestamp_ns, (unsigned long long)sample_ns, (long long)query_ns,
		    (unsigned long long)device_time_ns, out->pose.orientation.x, out->pose.orientation.y,
		    out->pose.orientation.z, out->pose.orientation.w);
	}
	return XRT_SUCCESS;
}

static xrt_result_t
compute_distortion(struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *result)
{
	struct arpara_hmd *h = (struct arpara_hmd *)xdev;
	return arpara_optics_compute(&h->optics, view, u, v, result) ? XRT_SUCCESS : XRT_ERROR_INPUT_UNSUPPORTED;
}

static xrt_result_t
get_mask(struct xrt_device *xdev, enum xrt_visibility_mask_type type, uint32_t view, struct xrt_visibility_mask **out)
{
	if (view >= xdev->hmd->view_count) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	u_visibility_mask_get_default(type, &xdev->hmd->distortion.fov[view], out);
	return XRT_SUCCESS;
}

static void
destroy(struct xrt_device *xdev)
{
	struct arpara_hmd *h = (struct arpara_hmd *)xdev;
	os_thread_helper_destroy(&h->thread); // Joins before closing HID/freeing fusion.
	U_LOG_I("arpara: closed HID, received %llu IMU samples", (unsigned long long)h->samples);
	os_hid_destroy(h->hid);
	m_imu_3dof_close(&h->fusion);
	os_mutex_destroy(&h->mutex);
	u_device_free(xdev);
}

struct xrt_device *
arpara_create(struct os_hid_device *hid)
{
	struct arpara_hmd *h = U_DEVICE_ALLOCATE(
	    struct arpara_hmd, (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE), 1, 0);
	if (h == NULL) {
		os_hid_destroy(hid);
		return NULL;
	}
	h->hid = hid;
	if (os_mutex_init(&h->mutex) != 0) {
		os_hid_destroy(hid);
		u_device_free(&h->base);
		return NULL;
	}
	if (os_thread_helper_init(&h->thread) != 0) {
		os_mutex_destroy(&h->mutex);
		os_hid_destroy(hid);
		u_device_free(&h->base);
		return NULL;
	}
	m_imu_3dof_init(&h->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_300MS);
	h->orientation.w = 1;
	struct xrt_device *d = &h->base;
	d->name = XRT_DEVICE_GENERIC_HMD;
	d->device_type = XRT_DEVICE_TYPE_HMD;
	snprintf(d->str, sizeof(d->str), "arpara 5K");
	/* A label, not a fabricated hardware serial number. */
	snprintf(d->serial, sizeof(d->serial), "arpara 5K (USB HID)");
	d->inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	d->supported.orientation_tracking = debug_get_bool_option_arpara_imu();
	d->supported.position_tracking = false;
	d->update_inputs = u_device_noop_update_inputs;
	d->get_tracked_pose = get_pose;
	d->get_view_poses = u_device_get_view_poses;
	d->get_visibility_mask = get_mask;
	d->destroy = destroy;
	snprintf(d->tracking_origin->name, sizeof(d->tracking_origin->name), "arpara IMU (experimental)");
	d->tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	/* Placeholders solely for device registration: NOT calibrated panel/optics. */
	const struct u_device_simple_info info = {
	    .display = {2560, 1440, 0.13f, 0.073f},
	    .lens_horizontal_separation_meters = 0.065f,
	    .lens_vertical_position_meters = 0.0365f,
	    .fov = {1.570796327f, 1.570796327f},
	};
	if (!u_device_setup_split_side_by_side(d, &info)) {
		destroy(d);
		return NULL;
	}
	d->hmd->screens[0].nominal_frame_interval_ns = 16666667; // Placeholder 60 Hz.
	if (!arpara_optics_load_builtin(&h->optics)) {
		U_LOG_E("arpara: optical calibration failed; refusing an uncalibrated fallback");
		destroy(d);
		return NULL;
	}
	for (unsigned eye = 0; eye < 2; ++eye) {
		d->hmd->views[eye].display.w_pixels = h->optics.recommended_width;
		d->hmd->views[eye].display.h_pixels = h->optics.recommended_height;
		d->hmd->distortion.fov[eye] = h->optics.fov[eye];
	}
	d->compute_distortion = compute_distortion;
	d->hmd->distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	d->hmd->distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
	u_distortion_mesh_fill_in_compute(d);
	if (!d->hmd->distortion.mesh.vertices || !d->hmd->distortion.mesh.indices) {
		U_LOG_E("arpara: distortion mesh generation failed");
		destroy(d);
		return NULL;
	}
	U_LOG_I("arpara: loaded DISTORT_POLY3 optics, source SHA-256=%s, eye target=%ux%u; mesh generated",
	        arpara_optics_source_hash(), h->optics.recommended_width, h->optics.recommended_height);
	U_LOG_W(
	    "arpara: optics from supplied unit calibration; RGB shares green where color fields are absent. "
	    "IPD, IMU-to-eye transform and provisional 2560x1440/60Hz registration geometry remain uncalibrated; "
	    "DRM physical mode is selected separately.");
	if (d->supported.orientation_tracking && os_thread_helper_start(&h->thread, reader, h) != 0) {
		U_LOG_E("arpara: failed to start HID reader");
		destroy(d);
		return NULL;
	}
	if (!d->supported.orientation_tracking) {
		U_LOG_W("arpara: ARPARA_IMU=false: fixed identity orientation for registration testing");
	}
	return d;
}

int
arpara_found(struct xrt_prober *xp,
             struct xrt_prober_device **devices,
             size_t device_count,
             size_t index,
             cJSON *attached_data,
             struct xrt_device **out_xdevs)
{
	if (index >= device_count || devices[index]->vendor_id != 0x2c30 || devices[index]->product_id != 0x1050) {
		return 0;
	}
	struct os_hid_device *hid = NULL;
	int ret = xrt_prober_open_hid_interface(xp, devices[index], 0, &hid);
	if (ret != 0 || hid == NULL) {
		U_LOG_E("arpara: failed to open HID interface 0 (%d); check hidraw permissions and exclusive access",
		        ret);
		if (hid != NULL) {
			os_hid_destroy(hid);
		}
		return -1;
	}
	U_LOG_I("arpara: opened 2c30:1050 HID interface 0");
	out_xdevs[0] = arpara_create(hid);
	return out_xdevs[0] != NULL ? 1 : -1;
}
