// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
// Wrap USB/context entry points, but run the actual Monado libsurvive adapter, device and history.
#include "catch_amalgamated.hpp"
#include "survive/survive_interface.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "os/os_time.h"
#define SURVIVE_ENABLE_FULL_API
#include <survive_api.h>
#include <array>
#include <string>
#include <thread>
#include <chrono>
#include <sys/time.h>
#include <pthread.h>
#include <cerrno>

namespace {
struct Object
{
	std::string json;
	SurviveSimpleObject_type type;
};
struct Context
{
	std::array<Object, 20> objects;
	int count = 0, config = 0;
	bool pose_sent = false;
	bool failure_armed = false;
};
int mode = 0, closed = 0;
int mutex_failure_countdown = 0;
} // namespace

extern "C" {
int
__real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int
__wrap_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	if (mutex_failure_countdown > 0 && --mutex_failure_countdown == 0)
		return EAGAIN;
	return __real_pthread_mutex_init(mutex, attr);
}
SurviveSimpleContext *
__wrap_survive_simple_init(int, char *const *)
{
	if (mode < 0)
		return nullptr;
	auto *c = new Context;
	c->count = mode == 20 ? 20 : (mode == 0 ? 0 : 1);
	for (int i = 0; i < c->count; ++i) {
		std::string model = mode == 2 || mode == 20
		                        ? "Tundra Tracker"
		                        : (mode == 3 ? "Other HMD" : (mode == 6 ? "Vive MV" : "arpara"));
		c->objects[i].json = "{\"model_number\":\"" + model + "\",\"device_serial_number\":\"TEST-" +
		                     std::to_string(i) +
		                     "\",\"manufacturer\":\"arpara\",\"device_class\":\"hmd\","
		                     "\"device_vid\":10462,\"device_pid\":8960}";
		c->objects[i].type = mode == 2 || mode == 20 ? SurviveSimpleObject_OBJECT : SurviveSimpleObject_HMD;
	}
	return reinterpret_cast<SurviveSimpleContext *>(c);
}
void
__wrap_survive_simple_start_thread(SurviveSimpleContext *)
{}
void
__wrap_survive_simple_close(SurviveSimpleContext *ctx)
{
	++closed;
	delete reinterpret_cast<Context *>(ctx);
}
size_t
__wrap_survive_simple_get_object_count(SurviveSimpleContext *ctx)
{
	return reinterpret_cast<Context *>(ctx)->count;
}
const SurviveSimpleObject *
__wrap_survive_simple_get_first_object(SurviveSimpleContext *ctx)
{
	auto *c = reinterpret_cast<Context *>(ctx);
	return c->count ? reinterpret_cast<SurviveSimpleObject *>(&c->objects[0]) : nullptr;
}
const SurviveSimpleObject *
__wrap_survive_simple_get_next_object(SurviveSimpleContext *ctx, const SurviveSimpleObject *obj)
{
	auto *c = reinterpret_cast<Context *>(ctx);
	auto *next = reinterpret_cast<const Object *>(obj) + 1;
	return next < c->objects.data() + c->count ? reinterpret_cast<const SurviveSimpleObject *>(next) : nullptr;
}
SurviveSimpleObject_type
__wrap_survive_simple_object_get_type(const SurviveSimpleObject *obj)
{
	return reinterpret_cast<const Object *>(obj)->type;
}
const char *
__wrap_survive_simple_object_name(const SurviveSimpleObject *)
{
	return "fixture";
}
const char *
__wrap_survive_simple_json_config(const SurviveSimpleObject *obj)
{
	return reinterpret_cast<const Object *>(obj)->json.c_str();
}
SurviveSimpleEventType
__wrap_survive_simple_next_event(SurviveSimpleContext *ctx, SurviveSimpleEvent *event)
{
	auto *c = reinterpret_cast<Context *>(ctx);
	*event = {};
	if (c->config == c->count) {
		if (!c->failure_armed && (mode == 4 || mode == 5)) {
			c->failure_armed = true;
			mutex_failure_countdown = mode == 4 ? 1 : 2;
		}
		return SurviveSimpleEventType_None;
	}
	event->event_type = SurviveSimpleEventType_ConfigEvent;
	event->d.config_event.object = reinterpret_cast<SurviveSimpleObject *>(&c->objects[c->config++]);
	return event->event_type;
}
SurviveSimpleEventType
__wrap_survive_simple_wait_for_event(SurviveSimpleContext *ctx, SurviveSimpleEvent *event)
{
	auto *c = reinterpret_cast<Context *>(ctx);
	*event = {};
	if (!c->pose_sent && c->count) {
		c->pose_sent = true;
		event->event_type = SurviveSimpleEventType_PoseUpdateEvent;
		auto &pose = event->d.pose_event;
		pose.object = reinterpret_cast<SurviveSimpleObject *>(&c->objects[0]);
		pose.pose.Rot[0] = 1;
		pose.pose.Pos[0] = 1;
		pose.pose.Pos[1] = 2;
		pose.pose.Pos[2] = 3;
		timeval now{};
		gettimeofday(&now, nullptr);
		pose.time = now.tv_sec + now.tv_usec / 1000000.0;
		return event->event_type;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
	return SurviveSimpleEventType_None;
}
}

TEST_CASE("Real libsurvive adapter exposes an HMD-configured module as tracker and preserves 6DoF history")
{
	mode = 1;
	closed = 0;
	SECTION("arpara Gaming Kit HMD profile") {}
	SECTION("ordinary Tundra profile")
	{
		mode = 2;
	}
	SECTION("explicit serial selects another HMD profile")
	{
		mode = 3;
	}
	xrt_device *devices[XRT_MAX_DEVICES_PER_PROBE] = {};
	REQUIRE(survive_get_tracker_devices(nullptr, devices, mode == 3 ? "TEST-0" : "") == 1);
	auto *d = devices[0];
	CHECK(d->device_type == XRT_DEVICE_TYPE_GENERIC_TRACKER);
	CHECK(d->hmd == nullptr);
	CHECK(d->name == XRT_DEVICE_VIVE_TRACKER_TUNDRA);
	xrt_space_relation pose{};
	for (int i = 0; i < 100; ++i) {
		REQUIRE(xrt_device_get_tracked_pose(d, XRT_INPUT_GENERIC_TRACKER_POSE, os_monotonic_get_ns(), &pose) ==
		        XRT_SUCCESS);
		if (pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	CHECK(pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	CHECK(pose.pose.position.x == Catch::Approx(1));
	CHECK(pose.pose.position.y == Catch::Approx(3));
	CHECK(pose.pose.position.z == Catch::Approx(-2));
	xrt_device_destroy(&devices[0]);
	CHECK(closed == 1);
}

TEST_CASE("Real libsurvive adapter bounds devices and tears down empty contexts")
{
	closed = 0;
	mode = 0;
	int expected = 0, expected_closed = 1;
	SECTION("empty dongle") {}
	SECTION("init fails")
	{
		mode = -1;
		expected_closed = 0;
	}
	SECTION("unselected HMD is excluded")
	{
		mode = 3;
	}
	SECTION("capacity")
	{
		mode = 20;
		expected = -1; // The backend also reserves slots for controllers.
	}
	SECTION("system mutex initialization fails")
	{
		mode = 4;
	}
	SECTION("event thread initialization fails")
	{
		mode = 5;
	}
	xrt_device *devices[XRT_MAX_DEVICES_PER_PROBE + 1] = {};
	int count = survive_get_tracker_devices(nullptr, devices, "");
	if (expected < 0) {
		CHECK(count > 0);
		REQUIRE(count <= XRT_MAX_DEVICES_PER_PROBE);
	} else {
		REQUIRE(count == expected);
	}
	CHECK(devices[XRT_MAX_DEVICES_PER_PROBE] == nullptr);
	for (int i = 0; i < count; ++i)
		xrt_device_destroy(&devices[i]);
	CHECK(closed == expected_closed);
}

TEST_CASE("Normal libsurvive entry point retains Lighthouse HMD and tracker roles")
{
	mode = 6;
	closed = 0;
	bool hmd = true;
	SECTION("Vive HMD remains HMD") {}
	SECTION("Tundra remains generic tracker")
	{
		mode = 2;
		hmd = false;
	}
	xrt_device *devices[XRT_MAX_DEVICES_PER_PROBE] = {};
	vive_config *config = nullptr;
	REQUIRE(survive_get_devices(nullptr, devices, &config) == 1);
	CHECK(devices[0]->device_type == (hmd ? XRT_DEVICE_TYPE_HMD : XRT_DEVICE_TYPE_GENERIC_TRACKER));
	CHECK((devices[0]->hmd != nullptr) == hmd);
	CHECK((config != nullptr) == hmd);
	xrt_device_destroy(&devices[0]);
	CHECK(closed == 1);
}
