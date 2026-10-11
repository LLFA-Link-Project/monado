# Copyright 2018-2024, Collabora, Ltd.
# Copyright 2024-2026, NVIDIA CORPORATION.
#
# SPDX-License-Identifier: BSL-1.0

set(tests
    tests_cxx_wrappers
    tests_deque
    tests_generic_callbacks
    tests_glue_device
    tests_glue_traits
    tests_history_buf
    tests_id_ringbuffer
    tests_json
    tests_lowpass_float
    tests_lowpass_integer
    tests_pacing
    tests_quatexpmap
    tests_quat_change_of_basis
    tests_quat_swing_twist
    tests_rational
    tests_relation_chain
    tests_vector
    tests_worker
    tests_pose
    tests_vec3_angle
	)
if(XRT_HAVE_D3D11)
	list(APPEND tests tests_aux_d3d_d3d11 tests_comp_client_d3d11)
endif()
if(XRT_HAVE_D3D12)
	list(APPEND tests tests_aux_d3d_d3d12 tests_comp_client_d3d12)
endif()
if(XRT_HAVE_VULKAN)
	list(
		APPEND
		tests
		tests_comp_client_vulkan
		tests_comp_vulkan_formats
		tests_vk_queue_mutex
		tests_uv_to_tangent
		tests_render_pipeline_cache
		)
endif()
if(XRT_FEATURE_OPENXR)
	list(APPEND tests tests_input_transform)
endif()
if(XRT_HAVE_OPENGL
   AND XRT_HAVE_OPENGL_GLX
   AND XRT_HAVE_SDL2
   AND (WIN32 OR VK_USE_PLATFORM_XLIB_XRANDR_EXT)
	)
	set(_have_opengl_test ON)
	list(APPEND tests tests_comp_client_opengl)
endif()
if(XRT_BUILD_DRIVER_HANDTRACKING)
	list(APPEND tests tests_levenbergmarquardt)
endif()

if(XRT_BUILD_DRIVER_ARPARA)
	list(APPEND tests tests_arpara tests_arpara_optics)
endif()

if(XRT_HAVE_DRM_KMS AND XRT_MODULE_COMPOSITOR_MAIN)
	list(APPEND tests tests_drm_dma tests_drm_timing tests_drm_kms_fence tests_drm_geometry)
endif()

foreach(testname ${tests})
	add_executable(${testname} ${testname}.cpp)
	target_link_libraries(${testname} PRIVATE xrt-external-catch2)
	target_link_libraries(${testname} PRIVATE aux_util)
	add_test(NAME ${testname} COMMAND ${testname} --success --allow-running-no-tests)
	set_target_properties(${testname} PROPERTIES FOLDER monado_tests)
endforeach()

# For tests that require more than just aux_util, link those other libs down here.

target_link_libraries(tests_cxx_wrappers PRIVATE xrt-interfaces)
target_link_libraries(tests_glue_device PRIVATE xrt-interfaces aux_glue)
target_link_libraries(tests_glue_traits PRIVATE xrt-interfaces aux_glue)
target_link_libraries(tests_history_buf PRIVATE aux_math)
target_link_libraries(tests_lowpass_float PRIVATE aux_math)
target_link_libraries(tests_lowpass_integer PRIVATE aux_math)
target_link_libraries(tests_quatexpmap PRIVATE xrt-external-tinyceres aux_math)
target_link_libraries(tests_rational PRIVATE aux_math)
target_link_libraries(tests_relation_chain PRIVATE aux_math)
target_link_libraries(tests_pose PRIVATE aux_math)
target_link_libraries(tests_quat_change_of_basis PRIVATE aux_math)
target_link_libraries(tests_quat_swing_twist PRIVATE aux_math)
target_link_libraries(tests_vec3_angle PRIVATE aux_math)

if(XRT_BUILD_DRIVER_HANDTRACKING)
	target_link_libraries(
		tests_levenbergmarquardt
		PRIVATE
			aux_math
			t_ht_mercury_includes
			t_ht_mercury_kine_lm_includes
			t_ht_mercury
			t_ht_mercury_kine_lm
		)
endif()

if(XRT_HAVE_D3D11)
	target_link_libraries(tests_aux_d3d_d3d11 PRIVATE aux_d3d)
	target_link_libraries(tests_comp_client_d3d11 PRIVATE comp_client comp_mock)
endif()

if(XRT_HAVE_D3D12)
	target_link_libraries(tests_aux_d3d_d3d12 PRIVATE aux_d3d)
	target_link_libraries(tests_comp_client_d3d12 PRIVATE comp_client comp_mock)
endif()

if(XRT_HAVE_VULKAN)
	target_link_libraries(tests_comp_vulkan_formats PRIVATE comp_util aux_vk)
	target_link_libraries(
		tests_comp_client_vulkan PRIVATE comp_client comp_mock comp_util aux_vk
		)
	target_link_libraries(tests_uv_to_tangent PRIVATE aux_render)
	target_link_libraries(
		tests_render_pipeline_cache
		PRIVATE
			aux_render_cache
			comp_util
			aux_vk
			aux_shaders
			xrt-interfaces
		)
	get_target_property(_aux_shaders_binary_dir aux_shaders BINARY_DIR)
	target_include_directories(
		tests_render_pipeline_cache SYSTEM PRIVATE ${_aux_shaders_binary_dir}
		)
	add_dependencies(tests_render_pipeline_cache aux_shaders render_shader_specialization)
endif()

if(XRT_FEATURE_OPENXR)
	target_link_libraries(
		tests_input_transform PRIVATE st_oxr xrt-interfaces xrt-external-openxr
		)
endif()
if(_have_opengl_test)
	target_link_libraries(
		tests_comp_client_opengl PRIVATE comp_client comp_mock aux_ogl SDL2::SDL2
		)
	target_include_directories(tests_comp_client_opengl PRIVATE SDL2::SDL2)
endif()

if(XRT_HAVE_VULKAN AND XRT_HAVE_D3D11)
	target_link_libraries(tests_aux_d3d_d3d11 PRIVATE comp_util aux_vk)
endif()

if(XRT_HAVE_VULKAN AND XRT_HAVE_D3D12)
	target_link_libraries(tests_aux_d3d_d3d12 PRIVATE comp_util aux_vk)
endif()

if(XRT_BUILD_DRIVER_ARPARA)
	target_link_libraries(tests_arpara_optics PRIVATE drv_arpara drv_includes aux_math)
	target_compile_definitions(tests_arpara_optics PRIVATE ARPARA_OPTICS_FIXTURE="${PROJECT_SOURCE_DIR}/src/xrt/drivers/arpara/calibration/optics.json")
	target_link_libraries(tests_arpara PRIVATE drv_arpara drv_includes aux_os target_lists)
	target_include_directories(tests_arpara PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common)
	if(XRT_HAVE_LIBUDEV)
		target_link_libraries(tests_arpara PRIVATE ${UDEV_LIBRARIES})
	endif()
	add_test(NAME tests_arpara_fixed COMMAND ${CMAKE_COMMAND} -E env ARPARA_IMU=false
		${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:tests_arpara> "[fixed]")
	add_test(NAME tests_arpara_prediction COMMAND ${CMAKE_COMMAND} -E env ARPARA_POSE_PREDICTION_MS=10
		${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:tests_arpara>)
	set(_arpara_rust_test_args)
	set(_arpara_rust_test_env)
	if(ARPARA_RUST_TARGET)
		list(APPEND _arpara_rust_test_args --target ${ARPARA_RUST_TARGET})
	endif()
	if(CMAKE_CROSSCOMPILING)
		if(CMAKE_CROSSCOMPILING_EMULATOR)
			string(TOUPPER "${ARPARA_RUST_TARGET}" _arpara_cargo_target)
			string(REPLACE "-" "_" _arpara_cargo_target "${_arpara_cargo_target}")
			list(JOIN CMAKE_CROSSCOMPILING_EMULATOR " " _arpara_rust_runner)
			list(APPEND _arpara_rust_test_env "CARGO_TARGET_${_arpara_cargo_target}_RUNNER=${_arpara_rust_runner}")
		else()
			list(APPEND _arpara_rust_test_args --no-run)
		endif()
	endif()
	add_test(NAME tests_arpara_rust COMMAND ${CMAKE_COMMAND} -E env ${_arpara_rust_test_env}
		${ARPARA_CARGO} test --offline --locked ${_arpara_rust_test_args}
		--manifest-path "${PROJECT_SOURCE_DIR}/src/xrt/drivers/arpara/rust/Cargo.toml"
		--target-dir "${PROJECT_BINARY_DIR}/arpara-rust-tests")
endif()

if(XRT_HAVE_VULKAN)
	target_link_libraries(tests_vk_queue_mutex PRIVATE aux_vk)
endif()

if(TARGET tests_drm_dma)
	add_test(NAME tests_drm_latency_analysis COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests_drm_latency_analysis.py")
	target_link_libraries(tests_drm_geometry PRIVATE comp_drm_kms xrt-interfaces)
	target_link_libraries(tests_drm_dma PRIVATE comp_drm_dma)
	target_sources(tests_drm_kms_fence PRIVATE drm_kms_fence_fixture.c)
	target_link_libraries(tests_drm_kms_fence PRIVATE PkgConfig::LIBDRM)
	target_link_libraries(tests_drm_timing PRIVATE comp_drm_kms)
endif()
