cmake_minimum_required(VERSION 3.28...4.4)

project(Erebus
    VERSION 0.1.0
    DESCRIPTION "An x86-64 kernel"
    LANGUAGES C CXX ASM
)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)
endif ()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CompilerFlags)
include(CodeQuality)
include(IsoBuilder)
include(DocsBuilder)
include(QemuEmulator)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(klib)
add_subdirectory(kernel)
add_subdirectory(user)
add_subdirectory(tests)

# ------------------------------------------------------------------------------
# The bootable image, and ways to run it
# ------------------------------------------------------------------------------
add_uefi_iso(os_iso
    OUTPUT erebus.iso
    EFI_IMAGE boot/limine/limine-uefi-cd.bin
    FILES
    "${LIMINE_DIR}/limine-uefi-cd.bin" boot/limine/limine-uefi-cd.bin
    "${LIMINE_DIR}/BOOTX64.EFI" EFI/BOOT/BOOTX64.EFI
    "${PROJECT_SOURCE_DIR}/misc/limine.conf" boot/limine/limine.conf
    "$<TARGET_FILE:kernel>" boot/kernel.elf
    "$<TARGET_FILE:user>" boot/user.elf
)

add_qemu_targets(qemu
    ISO_TARGET os_iso
    ISO_FILE "${CMAKE_CURRENT_BINARY_DIR}/erebus.iso"
    OVMF_CODE "${OVMF_DIR}/ovmf-code-x86_64.fd"
    OVMF_VARS "${OVMF_DIR}/ovmf-vars-x86_64.fd"
)

# The same image around the test kernel, and the targets that run it.
add_uefi_iso(test_iso
    OUTPUT erebus-test.iso
    EFI_IMAGE boot/limine/limine-uefi-cd.bin
    FILES
    "${LIMINE_DIR}/limine-uefi-cd.bin" boot/limine/limine-uefi-cd.bin
    "${LIMINE_DIR}/BOOTX64.EFI" EFI/BOOT/BOOTX64.EFI
    "${PROJECT_SOURCE_DIR}/misc/limine.conf" boot/limine/limine.conf
    "$<TARGET_FILE:kernel_test>" boot/kernel.elf
    "$<TARGET_FILE:user>" boot/user.elf
)

add_kernel_test_targets(
    ISO_TARGET test_iso
    ISO_FILE "${CMAKE_CURRENT_BINARY_DIR}/erebus-test.iso"
    OVMF_CODE "${OVMF_DIR}/ovmf-code-x86_64.fd"
    OVMF_VARS "${OVMF_DIR}/ovmf-vars-x86_64.fd"
)

# ------------------------------------------------------------------------------
# API documentation: HTML, and JSON for tools
# ------------------------------------------------------------------------------
add_doxygen_docs(docs
    BRIEF "${PROJECT_DESCRIPTION}"
    INPUTS
    "${PROJECT_SOURCE_DIR}/kernel/include"
    "${PROJECT_SOURCE_DIR}/kernel/src"
    "${PROJECT_SOURCE_DIR}/klib/include"
    "${PROJECT_SOURCE_DIR}/klib/src"
    "${PROJECT_SOURCE_DIR}/tests/framework"
    EXCLUDE
    "${PROJECT_SOURCE_DIR}/kernel/include/boot/limine.h"
    "${PROJECT_SOURCE_DIR}/kernel/include/boot/elf.h"
    INCLUDE_PATH
    "${PROJECT_SOURCE_DIR}/kernel/include"
    "${PROJECT_SOURCE_DIR}/klib/include"
    "${PROJECT_SOURCE_DIR}/user/include"
    "${PROJECT_SOURCE_DIR}/user/src"
)
