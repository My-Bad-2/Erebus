# ==============================================================================
# Subproject: klibc (Kernel C Library)
# Description: Custom C library implementation for the kernel.
# ==============================================================================

file(GLOB_RECURSE KLIBC_SOURCES CONFIGURE_DEPENDS
		"src/*.c"
		"src/*.cpp"
		"src/*.S"
)

if (NOT KLIBC_SOURCES)
	message(WARNING "klibc: No source files found in ${CMAKE_CURRENT_SOURCE_DIR}/src")
endif ()

add_library(klibc STATIC ${KLIBC_SOURCES})

target_link_libraries(klibc PRIVATE os_kernel_flags)
target_link_libraries(klibc PRIVATE freestanding-cxx-hdrs)

target_include_directories(klibc SYSTEM PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

if (COMMAND apply_code_quality)
	apply_code_quality(klibc)
endif ()

if (EREBUS_BUILD_HOST_TESTS)
	add_host_test(klibc_tests
			"tests/string/memset.cpp"
			"tests/string/memcpy.cpp"
			"tests/string/memmove.cpp"
			"tests/string/memcmp.cpp"
			"tests/string/memchr.cpp"
			"tests/string/strlen.cpp"
			"tests/string/strcmp.cpp"
			"tests/string/strcpy.cpp"
			"tests/string/strcat.cpp"
			"tests/string/strchr.cpp"
			"tests/string/strspn.cpp"
			"tests/string/strpbrk.cpp"
			"tests/string/strtok.cpp"
	)
endif ()
