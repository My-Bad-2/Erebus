# ====================================================================================
# Module: CodeQuality
# Description: Attaches clang-tidy (on-build) and clang-format (on-demand) to targets
# ====================================================================================

if(NOT LLVM_PREFIX STREQUAL "")
	set(LINTER_SEARCH_PATHS "${LLVM_PREFIX}/bin")
endif()

find_program(CLANG_TIDY_PROGRAM NAMES clang-tidy PATHS ${LINTER_SEARCH_PATHS})
find_program(CLANG_FORMAT_PROGRAM NAMES clang-format PATHS ${LINTER_SEARCH_PATHS})

if (CLANG_TIDY_PROGRAM)
	message(STATUS "Code Quality: Found clang-tidy -> ${CLANG_TIDY_PROGRAM}")
else ()
	message(WARNING "Code Quality: clang-tidy not found.")
endif ()

if (CLANG_FORMAT_PROGRAM)
	message(STATUS "Code Quality: Found clang-format -> ${CLANG_FORMAT_PROGRAM}")

	if (NOT TARGET format)
		add_custom_target(format COMMENT "Running clang-format across all configured targets...")
	endif ()
else ()
	message(WARNING "Code Quality: clang-format not found.")
endif ()

# ==============================================================================
# Function: apply_code_quality
# Description: Enables clang-tidy during compilation and adds the target's
#              sources to the global 'format' command.
#
# Arguments:
#   TARGET_NAME - The CMake target to analyze/format
# ==============================================================================
function(apply_code_quality TARGET_NAME)
	if (CLANG_TIDY_PROGRAM)
		set_target_properties(${TARGET_NAME} PROPERTIES
				C_CLANG_TIDY "${CLANG_TIDY_PROGRAM};-p;${CMAKE_BINARY_DIR}"
				CXX_CLANG_TIDY "${CLANG_TIDY_PROGRAM};-p;${CMAKE_BINARY_DIR}"
		)
	endif ()

	if (CLANG_FORMAT_PROGRAM)
		get_target_property(TARGET_SOURCES ${TARGET_NAME} SOURCES)

		set(FORMATTABLE_SOURCES "")

		foreach (SOURCE ${TARGET_SOURCES})
			if (SOURCE MATCHES "\\.(c|cpp|cxx|cc|h|hpp|hxx)$")
				get_filename_component(ABS_PATH "${SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
				list(APPEND FORMATTABLE_SOURCES "${ABS_PATH}")
			endif ()
		endforeach ()

		if (FORMATTABLE_SOURCES)
			set(FORMAT_TARGET_NAME "format_${TARGET_NAME}")

			add_custom_target(
					${FORMAT_TARGET_NAME}
					COMMAND ${CLANG_FORMAT_PROGRAM} -i -style=file ${FORMATTABLE_SOURCES}
					COMMENT "Formatting ${TARGET_NAME}..."
					VERBATIM
			)

			add_dependencies(format ${FORMAT_TARGET_NAME})
		endif ()
	endif ()
endfunction()
