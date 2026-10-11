# LLFA-Link: arpara display + Lighthouse tracking

## Scope and evidence

Implemented on top of `8e38b4d32` (documentation successor of the requested
`3f0b6394c` baseline). Opt-in only. arpara HID/IMU/optics and the DRM renderer,
DMA-BUF, FIFO slots, sync FD and KMS restore code are unchanged.

The original optical JSON was read without modification. SHA-256:
`267b5215f0856749d77ef3ed0acf03bb5ce6f8e434c7a1b93a6bbd4fd27c070c`.
It identifies `model_number=arpara`, `manufacturer=arpara`, `device_class=hmd`,
`device_vid=10462 (28de)`, `device_pid=8960 (2300)`. The user confirms that it
was downloaded from the HMD-configured tracking module. Its serial and complete
calibration are not copied into the repository. The existing sanitized built-in
arpara optical calibration remains the display source.

## Cause

`vive_builder_estimate()` in `src/xrt/auxiliary/vive/vive_builder.c` treats
`VALVE_VID:VIVE_PRO_LHR_PID` as an Index HMD candidate; the latter is `0x2300`
in `vive_common.h`. The USB identity collides with the tracking module. This
estimate does not read `device_class` or optical calibration.

`p_create_system()` chooses the first builder with `certain.head`; it does not
use the estimate priority field. Legacy arpara creation is only a fallback.
`lighthouse_open_system_impl()` then requires an actually generated HMD and
returns `XRT_ERROR_DEVICE_CREATION_FAILED` if none exists. With libsurvive,
HMD-class objects go through `vive_config_parse()`, which rejects the unsupported
`arpara` HMD variant. The supplied error alone does not identify whether the
failed run used native Vive or libsurvive; both depend on this HMD-only opening
path. hidraw EACCES is a separate transport access failure.

## Implementation

`LLFA_TRACKER_OVERRIDE=1` enables the new `llfa` builder. It is registered before
other physical builders, after explicit simulation/debug builders. It claims
a head only when `2c30:1050` is present. Mode off or arpara absent leaves the
existing Lighthouse/legacy selection intact. Explicit configured debug builders
still take precedence.

The builder opens arpara once through `arpara_found()`, then calls the existing
libsurvive adapter through a tracking-only entry point when a Watchman/Tundra USB
transport is present. It bypasses the Lighthouse builder's HMD requirement and
Index camera/hand-tracking setup. A build without libsurvive can still use
arpara alone; with Lighthouse transport connected it reports the missing backend
instead of claiming 6DoF.

Tracking-only object classification is local to the libsurvive context:

- A normal `Tundra Tracker` model is accepted.
- The downloaded arpara HMD profile is accepted only with its model,
  manufacturer, class and VID/PID combination above.
- `LLFA_TRACKER_SERIAL` can explicitly select another HMD-configured object.
- Other Lighthouse HMDs are excluded from this opt-in arpara system only.
  The normal `survive_get_devices()` entry point still generates HMDs.

No device firmware or JSON calibration is rewritten. The accepted object is
created using the existing generic tracker device path, with
`XRT_INPUT_GENERIC_TRACKER_POSE`, no HMD parts and no display/eye transform.
The libsurvive solver's pose, coordinate conversion, velocity and relation history
remain in use. Automatic selection requires one candidate; multiple candidates,
unsupported pose input, or a missing explicitly selected serial fail clearly.
No initialized tracker and no explicit serial falls back to arpara IMU with a
warning. This can also indicate permissions/pairing trouble, not just absence.

The final head uses `multi_create_tracking_override(XRT_TRACKING_OVERRIDE_DIRECT,
arpara, tracker, XRT_INPUT_GENERIC_TRACKER_POSE, mount)`. DIRECT replaces the head
relation; ATTACHED would additionally compose arpara's own relation and is not
the rigid mounted-tracker operation required here. Display extent, optics, eye
FOV and relative IPD come from arpara. View poses and head queries use the same
override; requested timestamps reach the tracker unchanged. Capabilities and
tracking origin come from the tracker, and relation flags come from its history.
An empty history returns an invalid relation rather than uninitialized data.

`LLFA_TRACKER_IN_HMD=x,y,z,qx,qy,qz,qw` is the physical **tracker pose in HMD
coordinates**, translation in metres and normalized quaternion. Default:
`0,0,0,0,0,0,1`, an experimental uncalibrated mount. Non-finite, malformed or
zero-norm rotations are rejected. Monado's multi wrapper inverts this pose:
`world_from_hmd = world_from_tracker * inverse(hmd_from_tracker)`.
The tracker pose is the backend's IMU-reference pose; measure the mount relative
to that reference, not an assumed eye origin. Lighthouse-to-arpara IMU/eye
extrinsics are not claimed calibrated.

System ownership: the head slot is replaced by the wrapper, which owns arpara;
the system keeps a separate tracker slot. The wrapper does not own the tracker.
The backend releases its context with its last device. Empty contexts, bounded
output arrays, duplicate/late config events and initialization rollback are
handled to avoid unowned devices and double destruction. Devices arriving after
the static list is published require a restart; hotplug is not implemented.

This is a direct Lighthouse override, **not IMU/Lighthouse fusion**. The arpara
IMU thread continues normally for future work, but its 10 ms prediction option
does not predict the overridden head. Lighthouse uses its existing time mapping
and Monado relation history. No new timestamp remapping or extra extrapolation
is applied.

## Build and test evidence

Native x86_64 host; Orange Pi hardware and ARM64 toolchain unavailable here.
The existing `build` configuration was used. Full Monado builds were tested with
libsurvive disabled and enabled. The enabled check used a temporary core build of
[upstream libsurvive](https://github.com/collabora/libsurvive) at
`23f03665070549f0c13a6e768dfaf7764f75c417`, installed under `/tmp`; it is a
compile/test dependency, not an installed Orange Pi tracking deployment.

Tests include:

- opt-in and arpara-presence estimation, arpara-only, failed arpara opening,
  empty dongle, tracker integration, ambiguity and serial matching;
- mount parsing, inverse translation under rotation, requested timestamps,
  invalid/tracked flags, optics/view delegation and independent destruction;
- the real Monado libsurvive adapter with only USB/context API calls wrapped:
  HMD-shaped arpara profile, normal Tundra, explicit serial selection, 6DoF pose
  history/coordinate conversion, empty contexts, capacity limits, injected
  mutex/thread initialization failures and normal Vive HMD/Tundra roles;
- existing arpara protocol/Rust/IMU/optics/prediction and DRM regression tests.

Final counts: 40/40 with libsurvive enabled; 39/39 with it disabled.
USB discovery, Lighthouse solver accuracy, ARM64 build, physical mount offset,
OpenXR display and Orange Pi teardown still require hardware verification.

## Orange Pi procedure

Keep the existing working build/configuration as a rollback reference. Ensure
`pkg-config --modversion survive` succeeds and that the libsurvive installation
has its Vive USB driver and solver enabled. If missing, install/build libsurvive
according to its upstream instructions; LLFA does not silently substitute the
native orientation-only Vive driver.

```sh
cd ~/monado
pkg-config --modversion survive
cmake -S . -B build -DXRT_BUILD_DRIVER_ARPARA=ON \
  -DXRT_BUILD_DRIVER_SURVIVE=ON -DXRT_HAVE_DRM_KMS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure -j2
```

Connect/pair the tracking module before startup and provide normal user hidraw
access through appropriate udev rules. The [libsurvive README](https://github.com/collabora/libsurvive#quick-start)
describes its rules; verify coverage for the actual devices. Do not run Monado
as root or change all hidraw nodes to mode 666.

```sh
LLFA_TRACKER_OVERRIDE=1 \
LLFA_TRACKER_IN_HMD=0,0,0,0,0,0,1 \
SURVIVE_LOG=debug \
XRT_TRACKING_ORIGIN_OFFSET_Y=0 \
XRT_COMPOSITOR_FORCE_DRM=1 \
XRT_COMPOSITOR_DRM_DEVICE=/dev/dri/card0 \
XRT_COMPOSITOR_DRM_CONNECTOR=DP-1 \
XRT_COMPOSITOR_DRM_MODE=3840x1920@90.001 \
XRT_COMPOSITOR_DRM_SYNC_FD=1 \
XRT_COMPOSITOR_COMPUTE=0 \
XRT_COMPOSITOR_SCALE_PERCENTAGE=100 \
XRT_COMPOSITOR_DRM_PRE_FRAME_WAIT=1 \
XRT_COMPOSITOR_DRM_WAKE_BUDGET_US=6000 \
XRT_COMPOSITOR_FORCE_GPU_INDEX=0 \
XRT_COMPOSITOR_LOG=info \
./build/src/xrt/targets/service/monado-service 2>&1 | tee -i /tmp/monado-llfa.log
```

For multiple trackers or strict selection, additionally set
`LLFA_TRACKER_SERIAL='the-module-device_serial_number'` on the service process.
Use the serial in the downloaded config/device log, not a dongle's serial.
The previous synthetic 1.6 m tracking-origin height is set to zero above because
Lighthouse supplies real position. Establish the physical floor/origin using
Lighthouse calibration; don't layer the old 3DoF height onto it blindly.

Run the existing `hello_xr -G Vulkan` command in another terminal with the same
runtime manifest as before. Check that logs show `Using builder llfa`, initialized
arpara and tracker, selected tracker, and `head assigned to arpara wrapper`.
Check only one HMD role, no tracker HMD parts, real position changes when moving
the assembly, stable mount under rotation, unchanged optics, zero CPU pixel copy
and release fence waits, and KMS restore on normal exit. Test with arpara alone,
then with both, then mode off. Mode off intentionally restores the previous
selection, including the known USB collision for this unusual module.

## Changed files

- `src/xrt/targets/common/target_builder_llfa.c`
- `src/xrt/targets/common/target_llfa.c`, `target_llfa.h`
- `src/xrt/targets/common/target_lists.c`, `target_builder_interface.h`, `CMakeLists.txt`
- `src/xrt/drivers/survive/survive_driver.c`, `survive_interface.h`, `survive_internal.h`, `survive_llfa.h`
- `tests/tests_llfa.cpp`, `tests/tests_survive_llfa.cpp`, `tests/CMakeLists.txt`
- `doc/arpara-llfa-link.md`
