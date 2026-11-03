# ==============================================================================
# The kernel image
#
# Everything but main.cpp is compiled once, into kernel_objects. The kernel
# links those with main.cpp; the test kernel in tests/ links the same objects
# with its own build of main.cpp and the tests.
# ==============================================================================

# The boot-time self-tests these enabled are now tests of the test kernel.
unset(EREBUS_MM_SELFTEST CACHE)
unset(EREBUS_SMP_SELFTEST CACHE)

# ------------------------------------------------------------------------------
# build_info.hpp: name, version and the commit it was configured from
# ------------------------------------------------------------------------------
find_package(Git QUIET)
set(BUILD_INFO_GIT_COMMIT "unknown")
if (GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE BUILD_INFO_GIT_COMMIT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
  )
endif ()

string(TOLOWER "${PROJECT_NAME}" BUILD_INFO_NAMESPACE)
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/include/generated")
configure_file("${PROJECT_SOURCE_DIR}/cmake/templates/build_info.hpp.in"
    "${generated_dir}/build_info.hpp" @ONLY)

# ------------------------------------------------------------------------------
# kernel_objects
# ------------------------------------------------------------------------------
set(KERNEL_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

file(GLOB_RECURSE kernel_sources CONFIGURE_DEPENDS "src/*.cpp" "src/*.c" "src/*.S")
file(GLOB_RECURSE kernel_headers CONFIGURE_DEPENDS "include/*.h" "include/*.hpp")
list(REMOVE_ITEM kernel_sources "${KERNEL_MAIN}")

add_library(kernel_objects OBJECT)
target_sources(kernel_objects
    PRIVATE ${kernel_sources}
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES ${kernel_headers}
    PUBLIC FILE_SET generated TYPE HEADERS BASE_DIRS "${generated_dir}"
    FILES "${generated_dir}/build_info.hpp"
)

# The C++ headers come before klib's: libstdc++'s <cstdlib> and friends reach klib's C headers
# through #include_next, which only searches the directories after their own.
target_link_libraries(kernel_objects PUBLIC
    erebus::kernel_abi
    erebus::warnings
    erebus::cxx_headers
    erebus::klib
    uacpi::uacpi
    Zydis
    lgrt
)

erebus_code_quality(kernel_objects)

# ==============================================================================
# Function: add_kernel_image
# Usage:    add_kernel_image(<target> <sources...>)
#
# A bootable kernel ELF: kernel_objects, the given sources (a main.cpp among
# them) and the kernel's linker script.
# ==============================================================================
function(add_kernel_image target)
  set(linker_script "${PROJECT_SOURCE_DIR}/kernel/linker/linker.ld")

  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE kernel_objects)
  target_link_options(${target} PRIVATE "LINKER:--script=${linker_script}")
  set_target_properties(${target} PROPERTIES LINK_DEPENDS "${linker_script}")
endfunction()

add_kernel_image(kernel "${KERNEL_MAIN}")
erebus_code_quality(kernel)
