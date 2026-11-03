# ==============================================================================
# Module: CodeQuality
# Description: clang-tidy during the build (opt-in) and clang-format on demand,
#              for Erebus's own targets only; third-party code is never checked.
#
#   erebus_code_quality(<target>)   enrolls a target
#   format                          reformats every enrolled source
#   format_check                    fails if any of them is not formatted
#
# The checks themselves live in .clang-tidy: they look for bugs, not style, so
# whatever clang-tidy reports is worth reading.
# ==============================================================================

include_guard(GLOBAL)

option(EREBUS_CLANG_TIDY "Run clang-tidy on Erebus's own sources while building" OFF)

find_program(CLANG_TIDY_PROGRAM NAMES clang-tidy HINTS "${LLVM_PREFIX}/bin")
find_program(CLANG_FORMAT_PROGRAM NAMES clang-format HINTS "${LLVM_PREFIX}/bin")

if (EREBUS_CLANG_TIDY AND NOT CLANG_TIDY_PROGRAM)
  message(FATAL_ERROR "EREBUS_CLANG_TIDY is on, but clang-tidy was not found.")
endif ()

# Vendored files that keep their upstream formatting.
set(EREBUS_FORMAT_EXCLUDE_REGEX "/kernel/include/boot/limine\\.h$")

if (CLANG_FORMAT_PROGRAM)
  add_custom_target(format COMMENT "clang-format: reformatting the sources")
  add_custom_target(format_check COMMENT "clang-format: checking the sources")
else ()
  message(STATUS "Code Quality: clang-format not found; the format targets are not available.")
endif ()

# ==============================================================================
# Function: erebus_code_quality
# Usage:    erebus_code_quality(<target>)
# ==============================================================================
function(erebus_code_quality target)
  if (EREBUS_CLANG_TIDY)
    set(tidy_command
        "${CLANG_TIDY_PROGRAM}"
        --quiet
        "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
    set_target_properties(${target} PROPERTIES
        C_CLANG_TIDY "${tidy_command}"
        CXX_CLANG_TIDY "${tidy_command}"
    )
  endif ()

  if (NOT CLANG_FORMAT_PROGRAM)
    return()
  endif ()

  get_target_property(source_dir ${target} SOURCE_DIR)
  get_target_property(sources ${target} SOURCES)
  get_target_property(headers ${target} HEADER_SET)

  set(files "")
  foreach (file IN LISTS sources headers)
    if (NOT file MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$")
      continue()
    endif ()

    cmake_path(ABSOLUTE_PATH file BASE_DIRECTORY "${source_dir}" NORMALIZE)
    if (NOT file MATCHES "${EREBUS_FORMAT_EXCLUDE_REGEX}")
      list(APPEND files "${file}")
    endif ()
  endforeach ()

  list(REMOVE_DUPLICATES files)
  if (NOT files)
    return()
  endif ()

  add_custom_target(format_${target}
      COMMAND "${CLANG_FORMAT_PROGRAM}" -i --style=file ${files}
      COMMENT "clang-format: ${target}"
      VERBATIM
  )
  add_custom_target(format_check_${target}
      COMMAND "${CLANG_FORMAT_PROGRAM}" --dry-run --Werror --style=file ${files}
      COMMENT "clang-format: checking ${target}"
      VERBATIM
  )

  add_dependencies(format format_${target})
  add_dependencies(format_check format_check_${target})
endfunction()
