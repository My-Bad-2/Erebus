# ==============================================================================
# Module: DocsBuilder
# Description: API documentation with Doxygen, plus the same API as JSON made
#              from Doxygen's XML by cmake/scripts/doxygen_to_json.py:
#
#   add_doxygen_docs(<target> INPUTS <dirs or files...>
#                    [EXCLUDE <paths...>] [INCLUDE_PATH <dirs...>]
#                    [BRIEF <one-line description>])
#
#     <build>/<target>/html/index.html   browsable HTML
#     <build>/<target>/xml/              Doxygen's XML
#     <build>/<target>/api.json          the JSON
#     <build>/<target>/warnings.log      what Doxygen had to say
#
# Typst documents can still be built with add_typst_documentation().
#
# Missing tools are not a configure error: the targets that need them fail,
# saying what to install, when somebody builds them.
# ==============================================================================

include_guard(GLOBAL)

find_package(Doxygen 1.9 QUIET OPTIONAL_COMPONENTS dot)
find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
find_program(TYPST_PROGRAM typst)

if (DOXYGEN_FOUND)
  message(STATUS "Docs Builder: Found Doxygen ${DOXYGEN_VERSION} -> ${DOXYGEN_EXECUTABLE}")
else ()
  message(STATUS "Docs Builder: Doxygen not found; the docs target will fail if built.")
endif ()

# Doxyfile lists are space-separated, each path quoted.
function(_erebus_doxygen_list out_var)
  set(quoted "")
  foreach (item IN LISTS ARGN)
    string(APPEND quoted " \"${item}\"")
  endforeach ()
  string(STRIP "${quoted}" quoted)
  set(${out_var} "${quoted}" PARENT_SCOPE)
endfunction()

# ==============================================================================
# Function: add_doxygen_docs
# ==============================================================================
function(add_doxygen_docs target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "BRIEF" "INPUTS;EXCLUDE;INCLUDE_PATH")

  if (NOT ARG_INPUTS)
    message(FATAL_ERROR "add_doxygen_docs(${target}): INPUTS is required.")
  endif ()

  if (NOT DOXYGEN_FOUND OR NOT Python3_Interpreter_FOUND)
    add_custom_target(${target}
        COMMAND "${CMAKE_COMMAND}" -E echo "Cannot build ${target}: install Doxygen and Python 3."
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )
    return()
  endif ()

  set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}")
  set(json "${out}/api.json")
  set(stamp "${out}/doxygen.stamp")

  _erebus_doxygen_list(DOXYGEN_INPUT ${ARG_INPUTS})
  _erebus_doxygen_list(DOXYGEN_EXCLUDE ${ARG_EXCLUDE})
  _erebus_doxygen_list(DOXYGEN_INCLUDE_PATH ${ARG_INCLUDE_PATH})
  set(DOXYGEN_OUTPUT_DIR "${out}")
  set(DOXYGEN_PROJECT_BRIEF "${ARG_BRIEF}")
  if (DOXYGEN_DOT_FOUND)
    set(DOXYGEN_HAVE_DOT YES)
    cmake_path(GET DOXYGEN_DOT_EXECUTABLE PARENT_PATH DOXYGEN_DOT_PATH)
  else ()
    set(DOXYGEN_HAVE_DOT NO)
    set(DOXYGEN_DOT_PATH "")
  endif ()

  set(doxyfile "${CMAKE_CURRENT_BINARY_DIR}/${target}.Doxyfile")
  configure_file("${PROJECT_SOURCE_DIR}/cmake/templates/Doxyfile.in" "${doxyfile}" @ONLY)

  # Rerun Doxygen whenever a documented file changes.
  set(sources "")
  foreach (input IN LISTS ARG_INPUTS)
    if (IS_DIRECTORY "${input}")
      file(GLOB_RECURSE found CONFIGURE_DEPENDS
          "${input}/*.h" "${input}/*.hpp" "${input}/*.c" "${input}/*.cpp")
      list(APPEND sources ${found})
    else ()
      list(APPEND sources "${input}")
    endif ()
  endforeach ()

  # Stale pages of deleted files would otherwise stay in the output.
  add_custom_command(
      OUTPUT "${stamp}"
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${out}/html" "${out}/xml"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
      COMMAND "${DOXYGEN_EXECUTABLE}" "${doxyfile}"
      COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
      DEPENDS ${sources} "${doxyfile}"
      COMMENT "Doxygen: ${target} (problems go to ${out}/warnings.log)"
      VERBATIM
  )

  set(script "${PROJECT_SOURCE_DIR}/cmake/scripts/doxygen_to_json.py")
  add_custom_command(
      OUTPUT "${json}"
      COMMAND "${Python3_EXECUTABLE}" "${script}"
      --xml-dir "${out}/xml"
      --output "${json}"
      --project-name "${PROJECT_NAME}"
      --project-version "${PROJECT_VERSION}"
      --strip-prefix "${PROJECT_SOURCE_DIR}"
      DEPENDS "${stamp}" "${script}"
      COMMENT "Doxygen XML to JSON: ${json}"
      VERBATIM
  )

  add_custom_target(${target} DEPENDS "${json}")
endfunction()

# ==============================================================================
# Function: add_typst_documentation
# Usage:    add_typst_documentation(<target> DOCS_DIR <dir> OUTPUT_FILE <file>
#                                   FORMAT pdf|html [TEMPLATE <file.typ>])
#
# Gathers every .typ file under DOCS_DIR, in path order, into one master
# document after the optional TEMPLATE, and compiles it.
# ==============================================================================
function(add_typst_documentation target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DOCS_DIR;OUTPUT_FILE;FORMAT;TEMPLATE" "")

  if (NOT ARG_DOCS_DIR OR NOT ARG_OUTPUT_FILE OR NOT ARG_FORMAT)
    message(FATAL_ERROR "add_typst_documentation(${target}): DOCS_DIR, OUTPUT_FILE and FORMAT are required.")
  endif ()

  string(TOLOWER "${ARG_FORMAT}" format)
  if (NOT format MATCHES "^(pdf|html)$")
    message(FATAL_ERROR "add_typst_documentation(${target}): FORMAT must be pdf or html.")
  endif ()

  if (NOT TYPST_PROGRAM)
    add_custom_target(${target}
        COMMAND "${CMAKE_COMMAND}" -E echo "Cannot build ${target}: install typst."
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )
    return()
  endif ()

  file(GLOB_RECURSE typ_files CONFIGURE_DEPENDS "${ARG_DOCS_DIR}/*.typ")
  if (ARG_TEMPLATE)
    cmake_path(ABSOLUTE_PATH ARG_TEMPLATE NORMALIZE OUTPUT_VARIABLE template)
    list(REMOVE_ITEM typ_files "${template}")
  endif ()
  list(SORT typ_files)

  if (NOT typ_files)
    message(FATAL_ERROR "add_typst_documentation(${target}): no .typ files in ${ARG_DOCS_DIR}.")
  endif ()

  set(master "${CMAKE_CURRENT_BINARY_DIR}/${target}_master.typ")
  set(content "// Generated by add_typst_documentation(); edit the sources instead.\n\n")
  if (ARG_TEMPLATE)
    file(READ "${template}" template_content)
    string(APPEND content "${template_content}\n")
  endif ()
  foreach (file IN LISTS typ_files)
    string(APPEND content "#include \"${file}\"\n")
  endforeach ()
  # Rewritten only when it changes, so that an unchanged document is not rebuilt.
  file(WRITE "${master}.new" "${content}")
  file(COPY_FILE "${master}.new" "${master}" ONLY_IF_DIFFERENT)
  file(REMOVE "${master}.new")

  set(output "${CMAKE_CURRENT_BINARY_DIR}/${ARG_OUTPUT_FILE}")
  add_custom_command(
      OUTPUT "${output}"
      COMMAND "${TYPST_PROGRAM}" compile --root / --format ${format} "${master}" "${output}"
      DEPENDS ${typ_files} ${template} "${master}"
      COMMENT "Typst: ${output}"
      VERBATIM
  )
  add_custom_target(${target} DEPENDS "${output}")
endfunction()
