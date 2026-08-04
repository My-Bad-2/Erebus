# ==============================================================================
# Subproject: klib (Kernel C Library)
# Description: Custom C library implementation for the kernel.
# ==============================================================================

file(GLOB_RECURSE KLIB_SOURCES CONFIGURE_DEPENDS
		"src/*.c"
		"src/*.cpp"
		"src/*.S"
)

if (NOT KLIB_SOURCES)
	message(WARNING "klib: No source files found in ${CMAKE_CURRENT_SOURCE_DIR}/src")
endif ()

add_library(klib STATIC ${KLIB_SOURCES})

target_link_libraries(klib PRIVATE os_kernel_flags)
target_link_libraries(klib PRIVATE freestanding-cxx-hdrs)

target_include_directories(klib SYSTEM PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_precompile_headers(klib PUBLIC include/kformat.hpp)

if (COMMAND apply_code_quality)
	apply_code_quality(klib)
endif ()

add_typst_documentation(docs
		DOCS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/docs
		OUTPUT_FILE "docs.pdf"
		FORMAT pdf
)

if (EREBUS_BUILD_HOST_TESTS)
	add_host_test(klib_tests
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

			"tests/stdlib/atoi.cpp"
			"tests/stdlib/strtol.cpp"
			"tests/stdlib/qsort-bsearch.cpp"
			"tests/stdlib/abs.cpp"

			"tests/ctype.cpp"
			"tests/kformat.cpp"
			"tests/bitfield.cpp"
	)
endif ()
