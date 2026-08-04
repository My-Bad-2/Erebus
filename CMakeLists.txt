cmake_minimum_required(VERSION 3.25)

project(Erebus
		VERSION 0.1.0
		LANGUAGES C CXX ASM
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerFlags)
include(CodeQuality)
include(IsoBuilder)
include(DocsBuilder)
include(QemuEmulator)
include(HostTesting)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include subprojects
add_subdirectory(third_party)
add_subdirectory(klib)
add_subdirectory(kernel)

# Packaging ISO

set(ISO_FILENAME "erebus.iso")

add_uefi_iso(os_iso
		OUTPUT "${ISO_FILENAME}"
		EFI_IMAGE "boot/limine/limine-uefi-cd.bin"

		FILES
		"${LIMINE_DIR}/limine-uefi-cd.bin" "boot/limine/limine-uefi-cd.bin"
		"${LIMINE_DIR}/BOOTX64.EFI" "EFI/BOOT/BOOTX64.EFI"
		"${CMAKE_CURRENT_SOURCE_DIR}/misc/limine.conf" "boot/limine/limine.conf"
		"$<TARGET_FILE:kernel>" "boot/kernel.elf"
)

add_qemu_targets(qemu
		ISO_TARGET os_iso
		ISO_FILE "${CMAKE_CURRENT_BINARY_DIR}/${ISO_FILENAME}"
		OVMF_CODE "${OVMF_DIR}/ovmf-code-x86_64.fd"
		OVMF_VARS "${OVMF_DIR}/ovmf-vars-x86_64.fd"
		MEMORY "2G"
		SMP "2"
)
