// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "target_llfa.h"
#include "target_builder_interface.h"
#include "target_builder_helpers.h"
#include "arpara/arpara_interface.h"
#include "survive/survive_interface.h"
#include "xrt/xrt_prober.h"
#include "survive/survive_llfa.h"
#include "util/u_device.h"
#include "xrt/xrt_tracking.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

// These test-only providers exercise the real builder without USB access. Real arpara driver is tested
// separately by tests_arpara. Real libsurvive is also compiled in the full SURVIVE-enabled build.
static xrt_device *fixture_head = nullptr;
#ifdef XRT_BUILD_DRIVER_SURVIVE
static xrt_device *fixture_trackers[2] = {};
#endif
static int fixture_tracker_count = 0;
static int fixture_head_opens = 0;
extern "C" int
arpara_found(xrt_prober *, xrt_prober_device **, size_t, size_t, cJSON *, xrt_device **out)
{
	++fixture_head_opens;
	out[0] = fixture_head;
	fixture_head = nullptr;
	return out[0] != nullptr ? 1 : -1;
}
#ifdef XRT_BUILD_DRIVER_SURVIVE
extern "C" int
survive_get_tracker_devices(xrt_prober *, xrt_device **out, const char *)
{
	for (int i = 0; i < fixture_tracker_count; ++i) {
		out[i] = fixture_trackers[i];
		fixture_trackers[i] = nullptr;
	}
	return fixture_tracker_count;
}
#endif

struct TestProbe : xrt_prober
{
	xrt_prober_device arpara{}, dongle{};
	xrt_prober_device *devices[2] = {&arpara, &dongle};
	size_t count = 1;
	bool locked = false;
	TestProbe() : xrt_prober{}
	{
		arpara.vendor_id = 0x2c30;
		arpara.product_id = 0x1050;
		dongle.vendor_id = 0x28de;
		dongle.product_id = 0x2101;
		lock_list = [](xrt_prober *xp, xrt_prober_device ***out, size_t *count) {
			auto *p = static_cast<TestProbe *>(xp);
			p->locked = true;
			*out = p->devices;
			*count = p->count;
			return XRT_SUCCESS;
		};
		unlock_list = [](xrt_prober *xp, xrt_prober_device ***out) {
			static_cast<TestProbe *>(xp)->locked = false;
			*out = nullptr;
			return XRT_SUCCESS;
		};
	}
};

TEST_CASE("LLFA only claims a head when enabled and arpara USB is present")
{
	TestProbe p;
	auto *builder = t_builder_llfa_create();
	REQUIRE(builder != nullptr);
	auto *config = cJSON_CreateObject();
	xrt_builder_estimate estimate{};
	REQUIRE(xrt_builder_estimate_system(builder, config, &p, &estimate) == XRT_SUCCESS);
	CHECK(estimate.certain.head == (getenv("LLFA_TRACKER_OVERRIDE") != nullptr));
	CHECK_FALSE(p.locked);
	p.arpara.vendor_id = 0x28de;
	p.arpara.product_id = 0x2300; // Tundra / Index USB collision must not enable LLFA alone.
	estimate = {};
	REQUIRE(xrt_builder_estimate_system(builder, config, &p, &estimate) == XRT_SUCCESS);
	CHECK_FALSE(estimate.certain.head);
	xrt_builder_destroy(&builder);
	cJSON_Delete(config);
}

TEST_CASE("LLFA recognizes the downloaded arpara HMD profile without using its optics as tracker optics")
{
	char serial[32] = {};
	const char *profile = R"({"model_number":"arpara","manufacturer":"arpara","device_class":"hmd",
	    "device_vid":10462,"device_pid":8960,"device_serial_number":"TEST-MODULE",
	    "head":[{},{}],"tracking_to_eye_transform":[{},{}]})";
	REQUIRE(survive_llfa_tracker_config(profile, "", serial, sizeof(serial)));
	CHECK(std::strcmp(serial, "TEST-MODULE") == 0);
	CHECK(survive_llfa_tracker_config(R"({"model_number":"Tundra Tracker","device_serial_number":"T1"})", "",
	                                  serial, sizeof(serial)));
	const char *index = R"({"model_number":"Utah MP","device_serial_number":"HMD1"})";
	CHECK_FALSE(survive_llfa_tracker_config(index, "", serial, sizeof(serial)));
	CHECK(survive_llfa_tracker_config(index, "HMD1", serial, sizeof(serial)));
	CHECK_FALSE(survive_llfa_tracker_config(index, "OTHER", serial, sizeof(serial)));
	CHECK_FALSE(survive_llfa_tracker_config("{broken", "", serial, sizeof(serial)));
	CHECK_FALSE(survive_llfa_tracker_config("[]", "", serial, sizeof(serial)));
	CHECK_FALSE(survive_llfa_tracker_config(R"({"model_number":"Tundra Tracker"})", "", serial, sizeof(serial)));
	CHECK_FALSE(survive_llfa_tracker_config(profile, "", serial, 3));
}

TEST_CASE("LLFA mount is finite, normalized and strictly parsed")
{
	xrt_pose p{};
	REQUIRE(t_llfa_parse_mount(nullptr, &p));
	CHECK(p.orientation.w == 1);
	REQUIRE(t_llfa_parse_mount("0,.1,0,0,0,0,2", &p));
	CHECK(p.position.y == Catch::Approx(.1));
	CHECK(p.orientation.w == 1);
	CHECK_FALSE(t_llfa_parse_mount("0,0,0,0,0,0,0", &p));
	CHECK_FALSE(t_llfa_parse_mount("nan,0,0,0,0,0,1", &p));
	CHECK_FALSE(t_llfa_parse_mount("0,0,0,0,0,0,1,extra", &p));
	CHECK_FALSE(t_llfa_parse_mount("0,0,0", &p));
}

struct MockDevice : xrt_device
{
	xrt_input input{};
	xrt_tracking_origin origin{};
	xrt_space_relation sample{};
	int64_t requested = 0;
	int *destroyed;
	explicit MockDevice(int *destroy_count, bool head) : xrt_device{}, destroyed(destroy_count)
	{
		device_type = head ? XRT_DEVICE_TYPE_HMD : XRT_DEVICE_TYPE_GENERIC_TRACKER;
		name = head ? XRT_DEVICE_GENERIC_HMD : XRT_DEVICE_VIVE_TRACKER_TUNDRA;
		std::strcpy(serial, head ? "HEAD" : "TRACKER");
		input.name = head ? XRT_INPUT_GENERIC_HEAD_POSE : XRT_INPUT_GENERIC_TRACKER_POSE;
		inputs = &input;
		input_count = 1;
		tracking_origin = &origin;
		origin.type = XRT_TRACKING_TYPE_LIGHTHOUSE;
		supported.orientation_tracking = true;
		supported.position_tracking = !head;
		sample.pose = XRT_POSE_IDENTITY;
		destroy = [](xrt_device *d) {
			auto *m = static_cast<MockDevice *>(d);
			++*m->destroyed;
			delete m;
		};
		get_tracked_pose = [](xrt_device *d, xrt_input_name name, int64_t time, xrt_space_relation *out) {
			auto *m = static_cast<MockDevice *>(d);
			if (name != m->input.name)
				return XRT_ERROR_INPUT_UNSUPPORTED;
			m->requested = time;
			*out = m->sample;
			return XRT_SUCCESS;
		};
		compute_distortion = [](xrt_device *, uint32_t eye, float u, float v, xrt_uv_triplet *uv) {
			uv->r = {u + eye, v};
			uv->g = {u, v};
			uv->b = {u, v + eye};
			return XRT_SUCCESS;
		};
		get_view_poses = [](xrt_device *, const xrt_vec3 *, int64_t, xrt_view_type, uint32_t count,
		                    xrt_space_relation *head, xrt_fov *fov, xrt_pose *poses) {
			*head = {};
			for (uint32_t i = 0; i < count; ++i) {
				poses[i] = XRT_POSE_IDENTITY;
				poses[i].position.x = i ? .0325f : -.0325f;
				fov[i] = {-.7f, .7f, .7f, -.7f};
			}
			return XRT_SUCCESS;
		};
	}
};

TEST_CASE("LLFA serial selection rejects ambiguity, unsupported input and missing serial")
{
	int destroyed = 0;
	MockDevice a(&destroyed, false), b(&destroyed, false);
	std::strcpy(b.serial, "SECOND");
	xrt_device *devices[] = {&a, &b};
	CHECK(t_llfa_select_tracker(devices, 2, "") == -2);
	CHECK(t_llfa_select_tracker(devices, 2, "SECOND") == 1);
	CHECK(t_llfa_select_tracker(devices, 2, "MISSING") == -1);
	a.input.name = XRT_INPUT_GENERIC_HEAD_POSE;
	CHECK(t_llfa_select_tracker(devices, 2, "TRACKER") == -2);
}

TEST_CASE("LLFA DIRECT wrapper preserves optics, timestamp, flags and independent ownership")
{
	int head_destroyed = 0, tracker_destroyed = 0;
	auto *head = new MockDevice(&head_destroyed, true);
	auto *tracker = new MockDevice(&tracker_destroyed, false);
	xrt_hmd_parts geometry{};
	head->hmd = &geometry;
	tracker->sample.pose.position = {1, 2, 3};
	tracker->sample.pose.orientation = {0, 0, float(std::sqrt(.5)), float(std::sqrt(.5))};
	tracker->sample.relation_flags =
	    (xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                               XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                               XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	xrt_system_devices system{};
	system.static_xdevs[0] = head;
	system.static_xdevs[1] = tracker;
	system.static_xdev_count = 2;
	xrt_pose mount{};
	REQUIRE(t_llfa_parse_mount("0,.1,0,0,0,0,1", &mount));
	REQUIRE(t_llfa_wrap_head(&system, 0, 1, &mount) == XRT_SUCCESS);
	auto *wrapper = system.static_xdevs[0];
	CHECK(wrapper->hmd == &geometry);
	CHECK(wrapper->tracking_origin == &tracker->origin);
	CHECK(wrapper->supported.position_tracking);
	CHECK(wrapper->device_type == XRT_DEVICE_TYPE_HMD);
	xrt_space_relation pose{};
	REQUIRE(xrt_device_get_tracked_pose(wrapper, XRT_INPUT_GENERIC_HEAD_POSE, 123456789, &pose) == XRT_SUCCESS);
	CHECK(tracker->requested == 123456789);
	CHECK(pose.pose.position.x == Catch::Approx(1.1)); // inverse mount rotated by tracker orientation
	CHECK(pose.pose.position.y == Catch::Approx(2));
	CHECK(pose.relation_flags == tracker->sample.relation_flags);
	xrt_uv_triplet uv{};
	REQUIRE(xrt_device_compute_distortion(wrapper, 1, .4f, .5f, &uv) == XRT_SUCCESS);
	CHECK(uv.r.x == Catch::Approx(1.4));
	xrt_fov fovs[2]{};
	xrt_pose eyes[2]{};
	xrt_vec3 ipd{.065f, 0, 0};
	REQUIRE(xrt_device_get_view_poses(wrapper, &ipd, 123456789, XRT_VIEW_TYPE_STEREO, 2, &pose, fovs, eyes) ==
	        XRT_SUCCESS);
	CHECK(eyes[1].position.x - eyes[0].position.x == Catch::Approx(.065));
	CHECK(pose.pose.position.x == Catch::Approx(1.1));
	tracker->sample.relation_flags = XRT_SPACE_RELATION_BITMASK_NONE;
	REQUIRE(xrt_device_get_tracked_pose(wrapper, XRT_INPUT_GENERIC_HEAD_POSE, 123456789, &pose) == XRT_SUCCESS);
	CHECK_FALSE(pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	CHECK_FALSE(pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	xrt_device_destroy(&system.static_xdevs[0]);
	CHECK(head_destroyed == 1);
	CHECK(tracker_destroyed == 0);
	xrt_device_destroy(&system.static_xdevs[1]);
	CHECK(tracker_destroyed == 1);
}

TEST_CASE("LLFA builder creates arpara alone or an arpara wrapper and cleans every device exactly once")
{
	if (getenv("LLFA_TRACKER_OVERRIDE") == nullptr) {
		SUCCEED("LLFA disabled; opt-in builder test runs in tests_llfa_enabled");
		return;
	}
	int head_destroyed = 0, tracker_destroyed = 0;
	TestProbe probe;
	fixture_head = new MockDevice(&head_destroyed, true);
	fixture_head_opens = 0;
	fixture_tracker_count = 0;
	bool expect_wrapper = false, expect_error = false;
	SECTION("arpara only") {}
	SECTION("arpara HID open fails")
	{
		xrt_device_destroy(&fixture_head);
		expect_error = true;
	}
	SECTION("dongle present but no initialized tracker")
	{
		probe.count = 2;
#ifndef XRT_BUILD_DRIVER_SURVIVE
		expect_error = true;
#endif
	}
#ifdef XRT_BUILD_DRIVER_SURVIVE
	SECTION("arpara plus Tundra")
	{
		probe.count = 2;
		fixture_trackers[0] = new MockDevice(&tracker_destroyed, false);
		fixture_tracker_count = 1;
		expect_wrapper = true;
	}
	SECTION("multiple trackers require explicit selection")
	{
		probe.count = 2;
		fixture_trackers[0] = new MockDevice(&tracker_destroyed, false);
		fixture_trackers[1] = new MockDevice(&tracker_destroyed, false);
		std::strcpy(fixture_trackers[1]->serial, "SECOND");
		fixture_tracker_count = 2;
		expect_error = true;
	}
#endif
	auto *builder = t_builder_llfa_create();
	xrt_system_devices system{};
	t_builder_options roles{};
	auto result = reinterpret_cast<t_builder *>(builder)->open_system_static_roles(
	    builder, nullptr, &probe, nullptr, &system, nullptr, &roles);
	CHECK(result == (expect_error ? XRT_ERROR_DEVICE_CREATION_FAILED : XRT_SUCCESS));
	CHECK_FALSE(probe.locked);
	CHECK(fixture_head_opens == 1);
	if (!expect_error) {
		CHECK(roles.head == system.static_xdevs[0]);
		CHECK(roles.head->device_type == XRT_DEVICE_TYPE_HMD);
		CHECK(roles.head->supported.position_tracking == expect_wrapper);
	}
	for (uint32_t i = 0; i < system.static_xdev_count; ++i) {
		xrt_device_destroy(&system.static_xdevs[i]);
	}
	CHECK(head_destroyed == 1);
	CHECK(tracker_destroyed == fixture_tracker_count);
	xrt_builder_destroy(&builder);
}
