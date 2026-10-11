// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "arpara/arpara_interface.h"
#include "arpara/arpara_protocol.h"
#include "arpara/arpara_prediction.h"
#include "os/os_hid.h"
#include "os/os_time.h"
#include "util/u_device.h"
#include "target_lists.h"
#include "xrt/xrt_device.h"
#include <array>
#include <cmath>
#include <functional>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using Report = std::array<uint8_t, 64>;

TEST_CASE("arpara prediction is bounded by freshness and requested host time")
{
	const int64_t sample = 1000000000;
	CHECK(arpara_prediction_horizon(sample, sample + 1000000, sample + 9000000, 10000000) == 9000000);
	CHECK(arpara_prediction_horizon(sample, sample + 1000000, sample + 50000000, 10000000) == 10000000);
	CHECK(arpara_prediction_horizon(sample, sample + 1000000, sample + 9000000, 0) == 0);
	CHECK(arpara_prediction_horizon(sample, sample + 30000000, sample + 50000000, 10000000) == 0);
	CHECK(arpara_prediction_horizon(sample, sample - 1, sample + 1000000, 10000000) == 0);
	CHECK(arpara_prediction_horizon(sample, sample + 1000000, sample + 500000, 10000000) == 0);
	CHECK(arpara_prediction_horizon(0, sample, sample + 1000000, 10000000) == 0);
}

TEST_CASE("arpara history predicts world-space angular velocity with body-space rotation order")
{
	m_relation_history *history = nullptr;
	m_relation_history_create(&history);
	xrt_quat q = {0, 0, 0, 1};
	xrt_vec3 gyro = {0, 1, 0}; // rad/s
	auto rel = arpara_history_sample(&q, &gyro, true);
	REQUIRE(m_relation_history_push(history, &rel, 1000000000));
	xrt_space_relation result;
	REQUIRE(m_relation_history_get(history, 1010000000, &result) == M_RELATION_HISTORY_RESULT_PREDICTED);
	auto out = result.pose.orientation;
	CHECK(out.y == Catch::Approx(std::sin(.005)).margin(1e-6));
	CHECK(out.w == Catch::Approx(std::cos(.005)).margin(1e-6));
	CHECK(q.w == 1); // Prediction never feeds back into fusion.
	CHECK_FALSE(result.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	CHECK(result.pose.position.y == 0);
	// A 90 degree roll maps local +Y onto world -X: verify body-space order.
	q = {0, 0, float(std::sin(.7853981633974483)), float(std::cos(.7853981633974483))};
	rel = arpara_history_sample(&q, &gyro, true);
	CHECK(rel.angular_velocity.x == Catch::Approx(-1).margin(1e-6));
	m_relation_history_clear(history);
	REQUIRE(m_relation_history_push(history, &rel, 1000000000));
	REQUIRE(m_relation_history_get(history, 1010000000, &result) == M_RELATION_HISTORY_RESULT_PREDICTED);
	out = result.pose.orientation;
	CHECK(out.x == Catch::Approx(-q.z * std::sin(.005)).margin(1e-6));
	CHECK(out.y == Catch::Approx(q.w * std::sin(.005)).margin(1e-6));
	CHECK(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w == Catch::Approx(1));
	REQUIRE(m_relation_history_get(history, 1000000000, &result) == M_RELATION_HISTORY_RESULT_EXACT);
	out = result.pose.orientation;
	CHECK(out.x == q.x);
	CHECK(out.z == q.z);
	gyro = {};
	rel = arpara_history_sample(&q, &gyro, true);
	m_relation_history_clear(history);
	REQUIRE(m_relation_history_push(history, &rel, 1000000000));
	REQUIRE(m_relation_history_get(history, 1010000000, &result) == M_RELATION_HISTORY_RESULT_PREDICTED);
	out = result.pose.orientation;
	CHECK(out.z == Catch::Approx(q.z));
	REQUIRE(m_relation_history_push(history, &rel, 1020000000));
	CHECK(m_relation_history_get(history, 1010000000, &result) == M_RELATION_HISTORY_RESULT_INTERPOLATED);
	rel = arpara_history_sample(&q, &gyro, false);
	CHECK_FALSE(rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	CHECK_FALSE(rel.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	m_relation_history_destroy(&history);
}

static uint8_t
sum8(const uint8_t *p, size_t n)
{
	uint8_t sum = 0;
	for (size_t i = 0; i < n; ++i)
		sum += p[i];
	return sum;
}
static void
checksum(Report &p)
{
	p[p[3]] = sum8(p.data(), p[3]);
}
static Report
imu(uint16_t counter, float gyro_y = 0)
{
	Report p{};
	p[1] = 1;
	p[2] = 0x0a;
	p[3] = 44;
	float values[9] = {0, 1, 0, 0, gyro_y, 0, 0, 0, 0};
	for (size_t i = 0; i < 9; ++i) {
		uint32_t bits;
		memcpy(&bits, &values[i], 4);
		for (size_t j = 0; j < 4; ++j)
			p[4 + 4 * i + j] = (uint8_t)(bits >> (j * 8));
	}
	p[40] = (uint8_t)counter;
	p[41] = (uint8_t)(counter >> 8);
	checksum(p);
	return p;
}

TEST_CASE("arpara command framing and strict report validation")
{
	uint8_t out[65];
	uint32_t timeout;
	REQUIRE(arpara_rs_startup(14, out, &timeout) == 15);
	CHECK(timeout == 1000);
	const uint8_t vector[] = {0, 0xaa, 1, 0x81, 8, 0xe8, 3, 0xc8, 0, 0xe7};
	CHECK(memcmp(out, vector, sizeof(vector)) == 0);
	for (size_t i = 10; i < sizeof(out); ++i)
		CHECK(out[i] == 0);


	auto p = imu(65530);
	arpara_rs_decoded s{};
	REQUIRE(arpara_rs_decode(p.data(), p.size(), &s));
	CHECK(s.counter == 65530);
	CHECK(s.values[1] == 1);
	for (size_t n = 0; n < 45; ++n)
		CHECK_FALSE(arpara_rs_decode(p.data(), n, &s));
	p[44] ^= 1;
	CHECK_FALSE(arpara_rs_decode(p.data(), p.size(), &s));
	p = imu(1);
	p[1] = 2;
	checksum(p);
	CHECK_FALSE(arpara_rs_decode(p.data(), p.size(), &s));
	p = imu(1);
	p[1] |= 0x10;
	checksum(p);
	CHECK(arpara_rs_decode(p.data(), p.size(), &s));
	CHECK(s.kind == 3);
	p = imu(1);
	p[38] = 0x80;
	p[39] = 0x7f; // Infinity in the last magnetic channel.
	checksum(p);
	CHECK_FALSE(arpara_rs_decode(p.data(), p.size(), &s));
	CHECK(arpara_rs_counter_delta_ns(4, 65530) == 1000000);
	CHECK(arpara_rs_counter_delta_ns(10, 10) == 0);
	CHECK(arpara_rs_counter_delta_ns(500, 10) == 0);
}

/* Tests use the real driver/Monado fusion with an in-memory transport, never hidraw. */
struct FakeHid
{
	os_hid_device base{};
	std::mutex mutex;
	std::deque<Report> incoming;
	std::vector<uint8_t> commands;
	std::atomic<unsigned> acks{0};
	std::atomic<unsigned> completed{0};
	std::atomic<bool> unplugged{false};
	bool &closed;
	int reject = -1;
	uint16_t counter = 65500;
	bool streaming = false;
	bool suppress_response = false;

	explicit FakeHid(bool &destroyed) : closed(destroyed)
	{
		base.read = [](os_hid_device *hid, uint8_t *out, size_t n, int timeout) {
			auto *f = (FakeHid *)hid;
			if (f->unplugged)
				return -1;
			{
				std::lock_guard<std::mutex> lock(f->mutex);
				if (!f->incoming.empty()) {
					auto p = f->incoming.front();
					f->incoming.pop_front();
					memcpy(out, p.data(), 64);
					return 64;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(f->streaming ? 1 : timeout));
			if (!f->streaming)
				return 0;
			f->counter = (uint16_t)(f->counter + 10);
			auto p = imu(f->counter, 90);
			memcpy(out, p.data(), 64);
			return 64;
		};
		base.write = [](os_hid_device *hid, const uint8_t *p, size_t n) {
			auto *f = (FakeHid *)hid;
			if (n != 65 || p[0] != 0 || p[1] != 0xaa || p[1 + p[4]] != sum8(p + 1, p[4]))
				return -1;
			std::lock_guard<std::mutex> lock(f->mutex);
			if (p[3] == 0x0b) {
				if (p[2] != 0x11 || p[4] != 5 || p[5] != 1 || p[6] != 0xcc)
					return -1;
				f->acks++;
				return 65;
			}
			f->commands.push_back(p[3]);
			if (f->suppress_response) {
				f->completed++;
				return 65;
			}
			if (p[3] == 0x8c) {
				Report status{};
				status[1] = 1;
				status[2] = 0x0b;
				status[3] = 9;
				status[4] = 0x80;
				checksum(status);
				f->incoming.push_back(status);
				f->incoming.push_back(imu(65530)); // IMU during response wait.
				f->incoming.push_back(imu(4));     // Wrap during startup.
			}
			Report response{};
			response[1] = 0x11;
			response[2] = p[3];
			response[3] = p[3] == 0x4a ? 7 : 5; // Observed variable query length.
			response[response[3] - 1] = p[3] == f->reject ? 0xff : 0;
			checksum(response);
			f->incoming.push_back(response);
			f->completed++;
			if (p[3] == 0x81)
				f->streaming = true;
			return 65;
		};
		base.destroy = [](os_hid_device *hid) {
			auto *f = (FakeHid *)hid;
			f->closed = true;
			delete f;
		};
	}
};

static bool
wait_until(const std::function<bool()> &predicate)
{
	for (int i = 0; i < 200; ++i) {
		if (predicate())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

TEST_CASE("arpara real HMD creation, startup, fusion and disconnect with fake HID")
{
	bool closed = false;
	auto *hid = new FakeHid(closed);
	struct FakeProber
	{
		xrt_prober base{};
		FakeHid *hid;
	} prober;
	prober.hid = hid;
	prober.base.open_hid_interface = [](xrt_prober *xp, xrt_prober_device *, int iface, os_hid_device **out) {
		if (iface != 0)
			return -1;
		*out = &((FakeProber *)xp)->hid->base;
		return 0;
	};
	xrt_prober_device usb{};
	usb.vendor_id = 0x2c30;
	usb.product_id = 0x1050;
	xrt_prober_device *devices[] = {&usb};
	xrt_device *d = nullptr;
	usb.product_id = 0x1051;
	CHECK(arpara_found(&prober.base, devices, 1, 0, nullptr, &d) == 0);
	CHECK(d == nullptr);
	usb.product_id = 0x1050;
	REQUIRE(arpara_found(&prober.base, devices, 1, 0, nullptr, &d) == 1);
	CHECK(std::string(d->str) == "arpara 5K");
	CHECK(d->device_type == XRT_DEVICE_TYPE_HMD);
	xrt_device *xdevs[] = {d};
	int head, eyes, face, left, right, gamepad;
	u_device_assign_xdev_roles(xdevs, 1, &head, &eyes, &face, &left, &right, &gamepad);
	CHECK(head == 0); // Same role assignment function as the legacy builder.
	CHECK(d->hmd->view_count == 2);
	CHECK((d->hmd->distortion.models & XRT_DISTORTION_MODEL_COMPUTE) != 0);
	CHECK((d->hmd->distortion.models & XRT_DISTORTION_MODEL_MESHUV) != 0);
	CHECK((d->hmd->distortion.models & XRT_DISTORTION_MODEL_NONE) == 0);
	REQUIRE(d->hmd->distortion.mesh.vertices != nullptr);
	REQUIRE(d->hmd->distortion.mesh.indices != nullptr);
	CHECK(d->hmd->views[0].display.w_pixels == 2544);
	CHECK(d->hmd->views[1].display.h_pixels == 2544);
	xrt_uv_triplet distortion{};
	CHECK(d->compute_distortion(d, 0, .5f, .5f, &distortion) == XRT_SUCCESS);
	CHECK(distortion.g.x == Catch::Approx(.5f));
	CHECK(distortion.g.y == Catch::Approx(.5f));
	CHECK(d->compute_distortion(d, 2, .5f, .5f, &distortion) == XRT_ERROR_INPUT_UNSUPPORTED);
	const auto &mesh = d->hmd->distortion.mesh;
	for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
		for (unsigned component = 0; component < 8; ++component)
			REQUIRE(std::isfinite(mesh.vertices[i * mesh.stride / sizeof(float) + component]));
	}

	CHECK_FALSE(d->supported.position_tracking);
	CHECK(wait_until([&] { return hid->completed == 15; }));
	CHECK(hid->acks == 1);
	{
		std::lock_guard<std::mutex> lock(hid->mutex);
		const std::vector<uint8_t> expected{0x8c, 0x8d, 0x80, 1,    0x7f, 0x7f, 0x7f, 2,
		                                    0x40, 0x47, 0x86, 0x4a, 0x4e, 0x4f, 0x81};
		CHECK(hid->commands == expected);
	}
	xrt_space_relation pose{};
	CHECK(wait_until([&] {
		d->get_tracked_pose(d, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &pose);
		return fabsf(pose.pose.orientation.y) > 0.01f;
	}));
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) != 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
	CHECK(pose.pose.position.x == 0);
	CHECK(pose.pose.position.y == 0);
	CHECK(pose.pose.position.z == 0);
	const char *prediction_option = getenv("ARPARA_POSE_PREDICTION_MS");
	if (prediction_option && atoi(prediction_option) > 0) {
		CHECK(wait_until([&] {
			d->get_tracked_pose(d, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns() + 5000000, &pose);
			return (pose.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) != 0;
		}));
		CHECK_FALSE(pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
		CHECK(std::isfinite(pose.pose.orientation.w));
	}
	CHECK(d->get_tracked_pose(d, XRT_INPUT_GENERIC_GRIP_POSE, 0, &pose) == XRT_ERROR_INPUT_UNSUPPORTED);
	hid->unplugged = true;
	CHECK(wait_until([&] {
		d->get_tracked_pose(d, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &pose);
		return !(pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	}));
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
	CHECK_FALSE(pose.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	CHECK(pose.pose.position.x == 0);
	CHECK(pose.pose.position.y == 0);
	CHECK(pose.pose.position.z == 0);
	CHECK(fabsf(pose.pose.orientation.y) > 0.01f); // Disconnect retains the IMU orientation.
	const auto &q = pose.pose.orientation;
	CHECK(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w == Catch::Approx(1.0f));
	xrt_device_destroy(&d);
	CHECK(closed);
}

TEST_CASE("arpara rejected initialization stops commands and frees HID")
{
	bool closed = false;
	auto *hid = new FakeHid(closed);
	hid->reject = 0x8c;
	auto *d = arpara_create(&hid->base);
	REQUIRE(d != nullptr);
	CHECK(wait_until([&] { return hid->completed == 1; }));
	xrt_device_destroy(&d);
	CHECK(closed);
}

TEST_CASE("arpara HID open errors fail cleanly")
{
	xrt_prober prober{};
	prober.open_hid_interface = [](xrt_prober *, xrt_prober_device *, int, os_hid_device **) { return -13; };
	xrt_prober_device usb{};
	usb.vendor_id = 0x2c30;
	usb.product_id = 0x1050;
	xrt_prober_device *devices[] = {&usb};
	xrt_device *d = nullptr;
	CHECK(arpara_found(&prober, devices, 1, 0, nullptr, &d) == -1);
	CHECK(d == nullptr);
}

TEST_CASE("arpara fixed registration mode sends no HID commands", "[.][fixed]")
{
	bool closed = false;
	auto *hid = new FakeHid(closed);
	auto *d = arpara_create(&hid->base);
	REQUIRE(d != nullptr);
	CHECK_FALSE(d->supported.orientation_tracking);
	CHECK_FALSE(d->supported.position_tracking);
	xrt_space_relation pose{};
	CHECK(d->get_tracked_pose(d, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &pose) == XRT_SUCCESS);
	CHECK(pose.pose.orientation.w == 1);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) == 0);
	CHECK(pose.pose.position.x == 0);
	CHECK(pose.pose.position.y == 0);
	CHECK(pose.pose.position.z == 0);
	xrt_space_relation head{};
	xrt_fov fovs[2]{};
	xrt_pose views[2]{};
	const xrt_vec3 eye_relation = {0.065f, 0, 0}; // Test input, not a calibrated IPD.
	CHECK(d->get_view_poses(d, &eye_relation, os_monotonic_get_ns(), XRT_VIEW_TYPE_STEREO, 2, &head, fovs,
	                        views) == XRT_SUCCESS);
	CHECK((head.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
	CHECK((head.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
	CHECK((head.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0);
	CHECK((head.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) == 0);
	CHECK(head.pose.position.x == 0);
	CHECK(head.pose.position.y == 0);
	CHECK(head.pose.position.z == 0);
	CHECK(head.pose.orientation.w == 1);
	CHECK(views[0].position.x == Catch::Approx(-0.0325f));
	CHECK(views[1].position.x == Catch::Approx(0.0325f));
	for (const auto &view : views) {
		CHECK(view.position.y == 0);
		CHECK(view.position.z == 0);
		CHECK(view.orientation.x == 0);
		CHECK(view.orientation.y == 0);
		CHECK(view.orientation.z == 0);
		CHECK(view.orientation.w == 1);
	}
	CHECK(hid->completed == 0);
	xrt_device_destroy(&d);
	CHECK(closed);
}

TEST_CASE("arpara production USB prober registration")
{
	bool found = false;
	for (auto *entry = target_entry_list; entry->found != nullptr; ++entry) {
		if (entry->vendor_id == 0x2c30 && entry->product_id == 0x1050) {
			CHECK(entry->found == arpara_found);
			found = true;
		}
	}
	CHECK(found);
}

TEST_CASE("arpara startup timeout keeps an untracked HMD and shutdown is bounded")
{
	bool closed = false;
	auto *hid = new FakeHid(closed);
	hid->suppress_response = true;
	auto *d = arpara_create(&hid->base);
	REQUIRE(d != nullptr);
	CHECK(wait_until([&] { return hid->completed == 1; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	CHECK(hid->completed == 1); // No unacknowledged follow-up startup requests.
	xrt_space_relation pose{};
	d->get_tracked_pose(d, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &pose);
	CHECK(pose.pose.orientation.w == 1);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) == 0);
	CHECK((pose.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) == 0);
	auto before = std::chrono::steady_clock::now();
	xrt_device_destroy(&d);
	CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250));
	CHECK(closed);
}

TEST_CASE("arpara destruction interrupts an outstanding startup response wait")
{
	bool closed = false;
	auto *hid = new FakeHid(closed);
	hid->suppress_response = true;
	auto *d = arpara_create(&hid->base);
	REQUIRE(d != nullptr);
	CHECK(wait_until([&] { return hid->completed == 1; }));
	auto before = std::chrono::steady_clock::now();
	xrt_device_destroy(&d);
	CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250));
	CHECK(closed);
}
