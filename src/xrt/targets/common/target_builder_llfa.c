// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#include "xrt/xrt_config_drivers.h"
#include "arpara/arpara_interface.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_misc.h"
#include "target_builder_helpers.h"
#include "target_builder_interface.h"
#include "target_llfa.h"
#ifdef XRT_BUILD_DRIVER_SURVIVE
#include "survive/survive_interface.h"
#endif
#include <stdlib.h>

DEBUG_GET_ONCE_BOOL_OPTION(llfa_enabled, "LLFA_TRACKER_OVERRIDE", false)
DEBUG_GET_ONCE_OPTION(llfa_serial, "LLFA_TRACKER_SERIAL", "")
DEBUG_GET_ONCE_OPTION(llfa_mount, "LLFA_TRACKER_IN_HMD", "0,0,0,0,0,0,1")

static bool
is_arpara(struct xrt_prober_device *d)
{
	return d->vendor_id == 0x2c30 && d->product_id == 0x1050;
}

static bool
is_lighthouse_transport(struct xrt_prober_device *d)
{
	return d->vendor_id == 0x28de &&
	       (d->product_id == 0x2101 || d->product_id == 0x2102 || d->product_id == 0x2300);
}

static xrt_result_t
llfa_estimate(struct xrt_builder *xb, cJSON *config, struct xrt_prober *xp, struct xrt_builder_estimate *estimate)
{
	if (!debug_get_bool_option_llfa_enabled()) {
		return XRT_SUCCESS;
	}
	struct xrt_prober_device **devices = NULL;
	size_t count = 0;
	xrt_result_t ret = xrt_prober_lock_list(xp, &devices, &count);
	if (ret != XRT_SUCCESS) {
		return ret;
	}
	for (size_t i = 0; i < count; i++) {
		if (is_arpara(devices[i])) {
			estimate->certain.head = true;
		}
	}
	return xrt_prober_unlock_list(xp, &devices);
}

static xrt_result_t
llfa_open(struct xrt_builder *xb,
          cJSON *config,
          struct xrt_prober *xp,
          struct xrt_tracking_origin *origin,
          struct xrt_system_devices *system,
          struct xrt_frame_context *xfctx,
          struct t_builder_options *roles)
{
	if (!debug_get_bool_option_llfa_enabled()) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	const char *serial = debug_get_option_llfa_serial();
	struct xrt_pose mount;
	if (!t_llfa_parse_mount(debug_get_option_llfa_mount(), &mount)) {
		U_LOG_E("LLFA: invalid LLFA_TRACKER_IN_HMD; expected metres x,y,z and quaternion x,y,z,w");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct xrt_prober_device **devices = NULL;
	size_t count = 0;
	xrt_result_t ret = xrt_prober_lock_list(xp, &devices, &count);
	if (ret != XRT_SUCCESS) {
		return ret;
	}
	bool transport = false;
	int found = 0;
	for (size_t i = 0; i < count; i++) {
		transport |= is_lighthouse_transport(devices[i]);
		if (is_arpara(devices[i]) && found == 0) {
			found = arpara_found(xp, devices, count, i, config, system->static_xdevs);
		}
	}
	if (found > 0) {
		system->static_xdev_count = found;
	}
	ret = xrt_prober_unlock_list(xp, &devices);
	if (ret != XRT_SUCCESS || found != 1) {
		U_LOG_E(
		    "LLFA: could not initialize arpara HMD (HID permission/exclusive access is independent of "
		    "selection)");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	U_LOG_I("LLFA: arpara HMD initialized");

	if (transport) {
#ifdef XRT_BUILD_DRIVER_SURVIVE
		// Backend has its own bounded output contract. Avoid writing directly into a partially filled system.
		struct xrt_device *tracked[XRT_MAX_DEVICES_PER_PROBE] = {0};
		int n = survive_get_tracker_devices(xp, tracked, serial);
		for (int i = 0; i < n; i++) {
			if (system->static_xdev_count == ARRAY_SIZE(system->static_xdevs)) {
				for (int j = i; j < n; j++) {
					xrt_device_destroy(&tracked[j]);
				}
				return XRT_ERROR_DEVICE_CREATION_FAILED;
			}
			system->static_xdevs[system->static_xdev_count++] = tracked[i];
			if (tracked[i]->name == XRT_DEVICE_VIVE_TRACKER_TUNDRA) {
				U_LOG_I("LLFA: Tundra tracker initialized: serial=%s", tracked[i]->serial);
			}
		}
#else
		U_LOG_E(
		    "LLFA: Lighthouse USB transport present, but libsurvive was not compiled. Enable "
		    "XRT_BUILD_DRIVER_SURVIVE");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
#endif
	}

	int tracker = t_llfa_select_tracker(system->static_xdevs, system->static_xdev_count, serial);
	if (tracker == -2 || (tracker == -1 && serial[0] != '\0')) {
		U_LOG_E(
		    "LLFA: tracker selection failed or ambiguous; specify LLFA_TRACKER_SERIAL and check "
		    "SURVIVE_LOG=debug");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	if (tracker >= 0) {
		U_LOG_I("LLFA: tracker selected: %s; creating DIRECT tracking override (no IMU fusion)",
		        system->static_xdevs[tracker]->serial);
		ret = t_llfa_wrap_head(system, 0, (uint32_t)tracker, &mount);
		if (ret != XRT_SUCCESS) {
			return ret;
		}
		U_LOG_I("LLFA: head assigned to arpara wrapper; tracker-in-HMD=(%g,%g,%g;%g,%g,%g,%g)",
		        mount.position.x, mount.position.y, mount.position.z, mount.orientation.x, mount.orientation.y,
		        mount.orientation.z, mount.orientation.w);
	} else {
		U_LOG_W(
		    "LLFA: no Tundra tracker initialized; using arpara IMU only (check hidraw permissions if "
		    "expected)");
	}

	int head, eyes, face, left, right, gamepad;
	u_device_assign_xdev_roles(system->static_xdevs, system->static_xdev_count, &head, &eyes, &face, &left, &right,
	                           &gamepad);
	roles->head = system->static_xdevs[0];
	roles->left = left >= 0 ? system->static_xdevs[left] : NULL;
	roles->right = right >= 0 ? system->static_xdevs[right] : NULL;
	roles->gamepad = gamepad >= 0 ? system->static_xdevs[gamepad] : NULL;
	return XRT_SUCCESS;
}

static void
llfa_destroy(struct xrt_builder *xb)
{
	free(xb);
}

struct xrt_builder *
t_builder_llfa_create(void)
{
	static const char *drivers[] = {
	    "arpara",
#ifdef XRT_BUILD_DRIVER_SURVIVE
	    "survive",
#endif
	};
	struct t_builder *b = U_TYPED_CALLOC(struct t_builder);
	if (b == NULL) {
		return NULL;
	}
	b->base.estimate_system = llfa_estimate;
	b->base.open_system = t_builder_open_system_static_roles;
	b->base.destroy = llfa_destroy;
	b->base.identifier = "llfa";
	b->base.name = "LLFA arpara + libsurvive tracking override (opt-in)";
	b->base.driver_identifiers = drivers;
	b->base.driver_identifier_count = ARRAY_SIZE(drivers);
	b->open_system_static_roles = llfa_open;
	return &b->base;
}
