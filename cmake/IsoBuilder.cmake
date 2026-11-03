# ==============================================================================
# Module: IsoBuilder
# Description: Bootable UEFI ISO images, built with xorriso.
#
# Without xorriso the project still configures and builds; only the ISO
# targets fail, with a message saying why, when somebody builds them.
# ==============================================================================

include_guard(GLOBAL)

set(_EREBUS_RUN_QUIETLY "${CMAKE_CURRENT_LIST_DIR}/scripts/run_quietly.cmake")

find_program(XORRISO_PROGRAM xorriso)

if (XORRISO_PROGRAM)
  message(STATUS "ISO Builder: Found xorriso -> ${XORRISO_PROGRAM}")
else ()
  message(STATUS "ISO Builder: xorriso not found; ISO targets will fail if built.")
endif ()

# ==============================================================================
# Function: add_uefi_iso
# Usage:    add_uefi_iso(<target>
#                        OUTPUT <file.iso>
#                        EFI_IMAGE <path of the FAT EFI image inside the ISO>
#                        [VOLUME_ID <label>]
#                        FILES <source> <destination> [<source> <destination>...])
#
# Copies each source (generator expressions allowed) to its destination path
# inside a staging tree, then packs the tree into a hybrid ISO whose appended
# EFI system partition is EFI_IMAGE.
# ==============================================================================
function(add_uefi_iso target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "OUTPUT;EFI_IMAGE;VOLUME_ID" "FILES")

  if (NOT ARG_OUTPUT OR NOT ARG_EFI_IMAGE)
    message(FATAL_ERROR "add_uefi_iso(${target}): OUTPUT and EFI_IMAGE are required.")
  endif ()

  list(LENGTH ARG_FILES file_count)
  math(EXPR odd "${file_count} % 2")
  if (file_count EQUAL 0 OR odd)
    message(FATAL_ERROR "add_uefi_iso(${target}): FILES takes <source> <destination> pairs.")
  endif ()

  if (NOT ARG_VOLUME_ID)
    string(TOUPPER "${PROJECT_NAME}" ARG_VOLUME_ID)
  endif ()

  set(iso "${CMAKE_CURRENT_BINARY_DIR}/${ARG_OUTPUT}")

  if (NOT XORRISO_PROGRAM)
    add_custom_target(${target}
        COMMAND "${CMAKE_COMMAND}" -E echo "Cannot build ${ARG_OUTPUT}: install xorriso."
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )
    return()
  endif ()

  set(root "${CMAKE_CURRENT_BINARY_DIR}/${target}_root")

  # A fresh staging tree each time, so files dropped from the list do not linger.
  set(commands
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${root}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${root}"
  )
  set(sources "")

  math(EXPR last "${file_count} - 1")
  foreach (index RANGE 0 ${last} 2)
    math(EXPR dest_index "${index} + 1")
    list(GET ARG_FILES ${index} src)
    list(GET ARG_FILES ${dest_index} dest)

    cmake_path(GET dest PARENT_PATH dest_dir)
    list(APPEND commands
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${root}/${dest_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${src}" "${root}/${dest}"
    )
    list(APPEND sources "${src}")
  endforeach ()

  # xorriso adds to an existing image instead of replacing it. It exits with an error for any
  # problem from SORRY up, since a file it could not add is not a warning, and its output only
  # shows when it fails.
  list(APPEND commands
      COMMAND "${CMAKE_COMMAND}" -E rm -f "${iso}"
      COMMAND "${CMAKE_COMMAND}" -P "${_EREBUS_RUN_QUIETLY}" -- "${XORRISO_PROGRAM}"
      -report_about SORRY
      -return_with SORRY 32
      -outdev "${iso}"
      -volid "${ARG_VOLUME_ID}"
      -map "${root}" /
      -joliet on
      -boot_image any partition_offset=16
      -boot_image any partition_table=on
      -append_partition 2 0xef "${root}/${ARG_EFI_IMAGE}"
      -boot_image any efi_path=--interval:appended_partition_2:all::
      -boot_image any emul_type=no_emulation
      -boot_image any platform_id=0xef
  )

  add_custom_command(
      OUTPUT "${iso}"
      ${commands}
      DEPENDS ${sources}
      COMMENT "Building UEFI ISO ${ARG_OUTPUT}"
      VERBATIM
  )

  add_custom_target(${target} ALL DEPENDS "${iso}")
  set_target_properties(${target} PROPERTIES EREBUS_ISO_FILE "${iso}")
endfunction()
