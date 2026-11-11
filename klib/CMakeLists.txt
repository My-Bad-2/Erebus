# ==============================================================================
# klib: the kernel's C library and formatting, as a static library
#
# Its include directory is a system one for the kernel, so that <string.h> and
# friends resolve to klib's own and not to a host C library. The unit tests in
# tests/ build klib again for the host.
# ==============================================================================

file(GLOB_RECURSE klib_sources CONFIGURE_DEPENDS "src/*.c" "src/*.cpp" "src/*.S")
file(GLOB_RECURSE klib_headers CONFIGURE_DEPENDS "include/*.h" "include/*.hpp")
file(GLOB_RECURSE klib_private_headers CONFIGURE_DEPENDS "src/*.h" "src/*.hpp")

add_library(klib STATIC)
add_library(erebus::klib ALIAS klib)

add_library(ulib STATIC)
add_library(erebus::ulib ALIAS ulib)

target_sources(klib
    PRIVATE ${klib_sources} ${klib_private_headers}
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES ${klib_headers}
)
set_target_properties(klib PROPERTIES SYSTEM TRUE)

target_sources(ulib
    PRIVATE ${klib_sources} ${klib_private_headers}
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES ${klib_headers}
)
set_target_properties(ulib PROPERTIES SYSTEM TRUE)

target_link_libraries(klib PRIVATE erebus::kernel_abi erebus::warnings erebus::cxx_headers)
target_precompile_headers(klib PUBLIC include/kformat.hpp)

target_link_libraries(ulib PRIVATE erebus::user_abi erebus::warnings erebus::cxx_headers)
target_precompile_headers(ulib PUBLIC include/kformat.hpp)

erebus_code_quality(klib)
