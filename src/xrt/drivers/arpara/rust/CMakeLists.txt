# Copyright 2026, Monado contributors.
# SPDX-License-Identifier: BSL-1.0
find_program(ARPARA_CARGO cargo REQUIRED)
set(ARPARA_PROTOCOL_SOURCE "${PROJECT_SOURCE_DIR}/src/external/vrg9080-protocol-rs")
if(NOT EXISTS "${ARPARA_PROTOCOL_SOURCE}/src/codec.rs")
	message(FATAL_ERROR "arpara requires git submodule update --init src/external/vrg9080-protocol-rs")
endif()
# Cross builds must explicitly select the Rust target matching the C++ toolchain.
set(ARPARA_RUST_TARGET "" CACHE STRING "Rust target triple for arpara (empty: native)")
if(CMAKE_CROSSCOMPILING AND NOT ARPARA_RUST_TARGET)
	message(FATAL_ERROR "Set ARPARA_RUST_TARGET to match the cross-compilation toolchain")
endif()
set(_rust_target_args)
set(_rust_target_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(_rust_library_dir "${_rust_target_dir}/release")
if(ARPARA_RUST_TARGET)
	list(APPEND _rust_target_args --target ${ARPARA_RUST_TARGET})
	set(_rust_library_dir "${_rust_target_dir}/${ARPARA_RUST_TARGET}/release")
endif()
set(_rust_library "${_rust_library_dir}/libmonado_arpara_bridge.a")
add_custom_command(
	OUTPUT "${_rust_library}"
	COMMAND ${ARPARA_CARGO} build --offline --locked --release
		--manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
		--target-dir "${_rust_target_dir}" ${_rust_target_args}
	DEPENDS Cargo.toml Cargo.lock src/lib.rs
		"${ARPARA_PROTOCOL_SOURCE}/src/codec.rs"
		"${ARPARA_PROTOCOL_SOURCE}/src/transport/protocol.rs"
	VERBATIM
)
add_custom_target(arpara_rust_build DEPENDS "${_rust_library}")
add_library(arpara_rust STATIC IMPORTED GLOBAL)
set_target_properties(arpara_rust PROPERTIES IMPORTED_LOCATION "${_rust_library}")
add_dependencies(arpara_rust arpara_rust_build)
target_link_libraries(arpara_rust INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
