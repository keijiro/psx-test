cmake_minimum_required(VERSION 3.21)

project(
	psx-homebrew-hello
	LANGUAGES C
	VERSION 1.0.0
	DESCRIPTION "PSn00bSDK two-voice wavetable synthesis demo"
)

find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(RUST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml")
set(RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
	set(RUST_PROFILE release)
	set(RUST_RELEASE_FLAG --release)
else()
	set(RUST_PROFILE debug)
	set(RUST_RELEASE_FLAG)
endif()
set(RUST_LIBRARY "${RUST_TARGET_DIR}/mipsel-sony-psx/${RUST_PROFILE}/libpsx_synth.a")

file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/rust/src/*.rs")
add_custom_command(
	OUTPUT "${RUST_LIBRARY}"
	COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
		"${CARGO_EXECUTABLE}" +nightly-2026-09-26 build
		--manifest-path "${RUST_MANIFEST}" --target mipsel-sony-psx
		-Z build-std=core --locked ${RUST_RELEASE_FLAG}
	DEPENDS ${RUST_SOURCES} "${RUST_MANIFEST}" "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock" "${CMAKE_CURRENT_SOURCE_DIR}/rust/rust-toolchain.toml"
	VERBATIM
)
add_custom_target(synth_rust DEPENDS "${RUST_LIBRARY}")

set(ELF2X "${CMAKE_CURRENT_SOURCE_DIR}/scripts/elf2x-rust.py")
psn00bsdk_add_executable(hello GPREL src/main.c)
add_dependencies(hello synth_rust)
target_include_directories(hello PRIVATE src)
target_link_libraries(hello PRIVATE "${RUST_LIBRARY}")
