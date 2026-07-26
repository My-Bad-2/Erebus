# ==============================================================================
# Module: DocsBuilder
# Description: Generates a master document and compiles hierarchical
#              Typst files into a single project using Typst CLI.
# ==============================================================================

find_program(TYPST_PROGRAM typst)

if (TYPST_PROGRAM)
	message(STATUS "Docs Builder: Found typst -> ${TYPST_PROGRAM}")
else ()
	message(WARNING "Docs Builder: typst not found. Documentation will not be built.")
endif ()

# ================================================================================
# Function: add_typst_documentation
# Arguments:
#   TARGET_NAME  - Name of the custom target
#   DOCS_DIR     - Root directory of the .typ files
#   OUTPUT_FILE  - Name of the final output file
#   FORMAT       - Output format (pdf, html5)
#   TEMPLATE_DIR - (Optional) Path to a .typ file containing global styling rules
# ================================================================================
function(add_typst_documentation TARGET_NAME)
	cmake_parse_arguments(ARG "" "DOCS_DIR;OUTPUT_FILE;FORMAT;TEMPLATE" "" ${ARGN})

	if (NOT ARG_DOCS_DIR OR NOT ARG_OUTPUT_FILE OR NOT ARG_FORMAT)
		message(FATAL_ERROR "add_typst_documentation: DOCS_DIR, OUTPUT_FILE, and FORMAT are required.")
	endif ()

	# Strictly enforce PDF and HTML as requested
	string(TOLOWER "${ARG_FORMAT}" FORMAT_LOWER)
	if (NOT FORMAT_LOWER STREQUAL "pdf" AND NOT FORMAT_LOWER STREQUAL "html")
		message(FATAL_ERROR "add_typst_documentation: FORMAT must be exactly 'pdf' or 'html'.")
	endif ()

	if (NOT TYPST_PROGRAM)
		return()
	endif ()

	file(GLOB_RECURSE TYP_FILES CONFIGURE_DEPENDS "${ARG_DOCS_DIR}/*.typ")

	# If the user placed their template inside the docs directory, we must remove
	# it from the auto-gathered list to prevent it from being included twice.
	if (ARG_TEMPLATE)
		get_filename_component(ABS_TEMPLATE "${ARG_TEMPLATE}" ABSOLUTE)
		list(REMOVE_ITEM TYP_FILES "${ABS_TEMPLATE}")
	endif ()

	if (NOT TYP_FILES)
		message(WARNING "No Typst files found in ${ARG_DOCS_DIR}")
		return()
	endif ()

	# Sort alphabetically to preserve docs/component/subcomponent hierarchy
	list(SORT TYP_FILES)

	set(MASTER_TYP "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_master.typ")
	set(MASTER_CONTENT "/* Auto-generated Master Typst File */\n\n")

	# Inject the raw template contents into the global scope
	if (ARG_TEMPLATE)
		file(READ "${ABS_TEMPLATE}" TEMPLATE_CONTENT)
		string(APPEND MASTER_CONTENT "/* --- USER TEMPLATE START --- */\n")
		string(APPEND MASTER_CONTENT "${TEMPLATE_CONTENT}\n")
		string(APPEND MASTER_CONTENT "/* --- USER TEMPLATE END --- */\n\n")
	endif ()

	# Inject includes for all discovered component files
	foreach (FILE ${TYP_FILES})
		# Convert path to standard forward slashes to prevent Typst string escape errors
		file(TO_CMAKE_PATH "${FILE}" SAFE_FILE_PATH)
		string(APPEND MASTER_CONTENT "#include \"${SAFE_FILE_PATH}\"\n")
	endforeach ()

	# Write the master file to the build directory
	file(WRITE "${MASTER_TYP}" "${MASTER_CONTENT}")

	set(FINAL_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${ARG_OUTPUT_FILE}")
	set(DEPENDENCIES ${TYP_FILES} "${MASTER_TYP}")

	if (ARG_TEMPLATE)
		list(APPEND DEPENDENCIES "${ABS_TEMPLATE}")
	endif ()

	set(BUILD_COMMANDS
			${TYPST_PROGRAM} compile
			"${MASTER_TYP}"
			"${FINAL_OUTPUT}"
	)

	add_custom_command(
			OUTPUT "${FINAL_OUTPUT}"
			DEPENDS ${DEPENDENCIES}
			COMMAND ${BUILD_COMMANDS}
			COMMENT "Building Typst ${FORMAT_LOWER} documentation: ${FINAL_OUTPUT}"
			VERBATIM
	)

	add_custom_target(${TARGET_NAME} ALL DEPENDS "${FINAL_OUTPUT}")
endfunction()
