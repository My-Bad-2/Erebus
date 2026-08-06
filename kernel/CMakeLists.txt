# ==============================================================================
# Subproject: kernel
# ==============================================================================

find_package(Git QUIET)
set(GIT_HASH "unknown")
if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
	execute_process(
			COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			OUTPUT_VARIABLE GIT_HASH
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
	)
endif ()

string(TOLOWER "${PROJECT_NAME}" PROJ_NS)

set(HEADER_CONTENT
		"#ifndef KERNEL_BUILD_INFO_HPP
#define KERNEL_BUILD_INFO_HPP 1

#include <string_view>

namespace ${PROJ_NS}::build {
	inline constexpr std::string_view os_name = \"${PROJECT_NAME}\";
	inline constexpr std::string_view os_version = \"${PROJECT_VERSION}\";
	inline constexpr std::string_view git_commit = \"${GIT_HASH}\";
}")

set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/include/generated/build_id.hpp")
file(CONFIGURE
		OUTPUT "${VERSION_HEADER}"
		CONTENT "${HEADER_CONTENT}"
)

file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
		"src/*.cpp"
		"src/*.c"
		"src/*.S"
		"include/*.h"
		"include/*.hpp"
)

if (NOT KERNEL_SOURCES)
	message(WARNING "Kernel: No source files found in ${CMAKE_CURRENT_SOURCE_DIR}")
endif ()

add_executable(kernel ${KERNEL_SOURCES})

target_link_libraries(kernel PRIVATE os_kernel_flags klib freestanding-cxx-hdrs uacpi)
target_include_directories(kernel PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/include"
		"${CMAKE_CURRENT_BINARY_DIR}/include/generated"
)

set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/linker.ld")
target_link_options(kernel PRIVATE "-T${LINKER_SCRIPT}")
set_target_properties(kernel PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")

if (COMMAND apply_code_quality)
	apply_code_quality(kernel)
endif ()
