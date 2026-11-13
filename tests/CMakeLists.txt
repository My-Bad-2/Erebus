# ==============================================================================
# The test images
#
# kernel_test is the kernel with its tests built in: the same objects as the
# kernel, its own build of main.cpp, and the tests under kernel/ and klib/.
# user_test is the root task with its tests built in: the same objects as the
# user image, its own build of user/src/main.cpp, and the tests under user/ and
# klib/. Both run on framework/ (see framework/ktest.hpp), each with its own
# half of the runner.
#
# The test ISO boots kernel_test with user_test as its root task. The kernel
# runs its own tests, then the user image's, and reports on the serial console.
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

file(GLOB_RECURSE framework_headers CONFIGURE_DEPENDS "framework/*.hpp")
file(GLOB_RECURSE kernel_tests CONFIGURE_DEPENDS "kernel/*.cpp" "kernel/*.hpp")
file(GLOB_RECURSE user_tests CONFIGURE_DEPENDS "user/*.cpp" "user/*.hpp")
# klib is in both images, built for each one's ABI, so its tests run in both.
file(GLOB_RECURSE klib_tests CONFIGURE_DEPENDS "klib/*.cpp" "klib/*.hpp")

# ------------------------------------------------------------------------------
# kernel_test
# ------------------------------------------------------------------------------
add_kernel_image(kernel_test
    "${PROJECT_SOURCE_DIR}/kernel/src/main.cpp"
    framework/ktest.cpp framework/kernel.cpp ${framework_headers}
    ${kernel_tests} ${klib_tests})
target_include_directories(kernel_test PRIVATE framework)
target_compile_definitions(kernel_test PRIVATE
    EREBUS_KERNEL_TESTS
    "$<$<BOOL:${EREBUS_KTEST_EXIT_PORT}>:EREBUS_KTEST_EXIT_PORT=${EREBUS_KTEST_EXIT_PORT}>"
)

# Compares the user ABI headers with the kernel's, so it sees both.
set_source_files_properties(kernel/core/user_abi.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/user/include")

erebus_code_quality(kernel_test)

# ------------------------------------------------------------------------------
# user_test
# ------------------------------------------------------------------------------
add_user_image(user_test
    "${PROJECT_SOURCE_DIR}/user/src/main.cpp"
    framework/ktest.cpp framework/user.cpp ${framework_headers}
    ${user_tests} ${klib_tests})
target_include_directories(user_test PRIVATE framework)
target_compile_definitions(user_test PRIVATE EREBUS_USER_TESTS)

erebus_code_quality(user_test)
