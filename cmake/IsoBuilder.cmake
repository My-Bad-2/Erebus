# ======================================================================
# Module: IsoBuilder
# Description: Cross-platform UEFI ISO Builder
# ======================================================================

find_program(XORRISO_PROGRAM xorriso)
find_program(OSCDIMG_PROGRAM oscdimg)

if (XORRISO_PROGRAM)
	message(STATUS "ISO Builder: Found xorriso -> ${XORRISO_PROGRAM}")
else ()
	message(WARNING "No ISO generation tool found. Install xorrsio.")
	return()
endif ()

# ===============================================================================
# Function: add_uefi_iso
# Description: Copies files to a staging root and packages them into a UEFI ISO.
#
# Arguments:
#   TARGET_NAME - The name of the CMake target
#   OUTPUT      - The name of the output ISO file
#   EFI_IMAGE   - The relative path inside the ISO to the FAT32 EFI boot image
#   FILES       - A flat list of Source -> Destination pairs
# ===============================================================================
function(add_uefi_iso TARGET_NAME)
	cmake_parse_arguments(ARG "" "OUTPUT;EFI_IMAGE" "FILES" ${ARGN})

	if (NOT ARG_OUTPUT)
		message(FATAL_ERROR "add_uefi_iso: OUTPUT is required.")
	endif ()

	if (NOT ARG_EFI_IMAGE)
		message(FATAL_ERROR "add_uefi_iso: EFI_IMAGE is required for UEFI boot.")
	endif ()

	set(ISO_ROOT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_root")
	set(FINAL_ISO "${CMAKE_CURRENT_BINARY_DIR}/${ARG_OUTPUT}")

	set(BUILD_COMMANDS
			${CMAKE_COMMAND} -E rm -rf "${ISO_ROOT}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${ISO_ROOT}"
	)

	set(DEPENDENCIES "")

	list(LENGTH ARG_FILES FILES_COUNT)
	math(EXPR FILES_MOD "${FILES_COUNT} % 2")
	if (NOT FILES_MOD EQUAL 0)
		message(FATAL_ERROR "add_uefi_iso: FILES must be provided in exact Source -> Destination pairs.")
	endif ()


	set(INDEX 0)
	while (INDEX LESS FILES_COUNT)
		list(GET ARG_FILES ${INDEX} SRC)
		math(EXPR INDEX "${INDEX} + 1")
		list(GET ARG_FILES ${INDEX} DEST)
		math(EXPR INDEX "${INDEX} + 1")

		get_filename_component(DEST_DIR "${ISO_ROOT}/${DEST}" DIRECTORY)
		list(APPEND BUILD_COMMANDS
				COMMAND ${CMAKE_COMMAND} -E make_directory "${DEST_DIR}"
				COMMAND ${CMAKE_COMMAND} -E copy "${SRC}" "${ISO_ROOT}/${DEST}"
		)

		list(APPEND DEPENDENCIES "${SRC}")
	endwhile ()

	list(APPEND BUILD_COMMANDS
			COMMAND ${XORRISO_PROGRAM}
			-outdev "${FINAL_ISO}"
			-volid "LIMINE_OS"
			-map "${ISO_ROOT}" /

			-joliet on

			-boot_image any partition_offset=16
			-boot_image any partition_table=on

			-append_partition 2 0xef "${ISO_ROOT}/${ARG_EFI_IMAGE}"
			-boot_image any efi_path=--interval:appended_partition_2:all::

			-boot_image any emul_type=no_emulation
			-boot_image any platform_id=0xef
	)

	add_custom_command(
			OUTPUT "${FINAL_ISO}"
			DEPENDS ${DEPENDENCIES}
			COMMAND ${BUILD_COMMANDS}
			COMMENT "Building UEFI ISO: ${FINAL_ISO}"
			VERBATIM
	)

	add_custom_target(${TARGET_NAME} ALL DEPENDS "${FINAL_ISO}")
endfunction()
