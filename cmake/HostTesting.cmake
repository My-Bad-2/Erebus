# ==============================================================================
# HostTesting.cmake
# Automatically fetches GoogleTest and provides functions for host execution
# ==============================================================================

option(EREBUS_BUILD_HOST_TESTS "Build host-side unit tests for algorithms and data structures" OFF)

if (EREBUS_BUILD_HOST_TESTS)
	enable_testing()
	message(STATUS "Host Testing: Enabled. Fetching GoogleTest...")

	include(FetchContent)
	FetchContent_Declare(
			googletest
			GIT_REPOSITORY https://github.com/google/googletest.git
			GIT_TAG v1.17.0
	)

	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
	set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL "")
	FetchContent_MakeAvailable(googletest)

	# --------------------------------------------------------------------------
	# Function: add_host_test
	# Usage: add_host_test(target_name source1.cpp source2.cpp ...)
	# --------------------------------------------------------------------------
	function(add_host_test TEST_NAME)
		add_executable(${TEST_NAME} ${ARGN})

		target_link_libraries(${TEST_NAME} PRIVATE gtest_main gtest pthread)

		target_compile_features(${TEST_NAME} PRIVATE cxx_std_23)

		target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

		include(GoogleTest)
		gtest_discover_tests(${TEST_NAME})

		message(STATUS "Host Testing: Registered test suite '${TEST_NAME}'")
	endfunction()
endif ()

# Example usage:
#if(EREBUS_BUILD_HOST_TESTS)
#	add_host_test(kernel_tests
#			"tests/foo.cpp"
#	)
#
#	target_include_directories(kernel_tests PRIVATE
#			"${CMAKE_CURRENT_SOURCE_DIR}/include"
#	)
#endif()