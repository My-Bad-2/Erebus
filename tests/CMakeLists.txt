# ==============================================================================
# The test kernel
#
# kernel_test is the kernel with its tests built in: the same objects as the
# kernel, its own build of main.cpp, and every test under this directory. It
# boots like the real kernel, runs the tests (see framework/ktest.hpp) and
# reports on the serial console.
#
#     cmake --build <build> --target check         # boot it once in QEMU
#     ctest --test-dir <build>                     # the same, through CTest
#     cmake --build <build> --target qemu_stress   # boot it on many machines
# ==============================================================================

# Options of the host-side unit tests these replace.
unset(EREBUS_BUILD_HOST_TESTS CACHE)
unset(EREBUS_TEST_SANITIZERS CACHE)

set(EREBUS_KTEST_EXIT_PORT "0xf4" CACHE STRING
    "I/O port of QEMU's isa-debug-exit device, where the test kernel reports its result; empty for none")

file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS "*.cpp")
file(GLOB_RECURSE test_headers CONFIGURE_DEPENDS "*.hpp")

add_kernel_image(kernel_test
    "${PROJECT_SOURCE_DIR}/kernel/src/main.cpp" ${test_sources} ${test_headers})
target_include_directories(kernel_test PRIVATE framework)
target_compile_definitions(kernel_test PRIVATE
    EREBUS_KERNEL_TESTS
    "$<$<BOOL:${EREBUS_KTEST_EXIT_PORT}>:EREBUS_KTEST_EXIT_PORT=${EREBUS_KTEST_EXIT_PORT}>"
)

erebus_code_quality(kernel_test)
