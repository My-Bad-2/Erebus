# ==============================================================================
# Third-Party Dependencies
# Description: Automatically initializes and shallow-clones git submodules.
# ==============================================================================

find_package(Git QUIET)

if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    message(STATUS "Third-Party: Checking and populating Git submodules...")

    # --init    : Initializes the submodule if it hasn't been registered yet.
    # --depth 1 : Instructs Git to fetch only the exact commit referenced by the
    #             subproject, stripping away the entire repository history.
    execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --depth 1
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE GIT_SUBMOD_RESULT
            OUTPUT_VARIABLE GIT_SUBMOD_OUTPUT
            ERROR_VARIABLE GIT_SUBMOD_ERROR
    )

    if (NOT GIT_SUBMOD_RESULT EQUAL "0")
        message(FATAL_ERROR
                "Third-Party: Failed to populate Git submodules.\n"
                "Command Output:\n${GIT_SUBMOD_OUTPUT}\n"
                "Command Error:\n${GIT_SUBMOD_ERROR}"
        )
    else ()
        message(STATUS "Third-Party: Submodules populated successfully.")
    endif ()
else ()
    message(WARNING "Third-Party: Git not found or project is not a git repository. Submodules must be managed manually.")
endif ()

add_subdirectory(limine)
add_subdirectory(ovmf)
add_subdirectory(uacpi-wrapper)

set(ZYDIS_FEATURE_FORMATTER OFF FORCE)
set(ZYAN_WHOLE_PROGRAM_OPTIMIZATION ON FORCE)
set(ZYAN_NO_LIBC ON FORCE)

add_subdirectory(zydis EXCLUDE_FROM_ALL)

target_compile_options(Zydis PRIVATE
        $<TARGET_PROPERTY:os_base_flags,INTERFACE_COMPILE_OPTIONS>
)

target_compile_options(Zycore PRIVATE
        $<TARGET_PROPERTY:os_base_flags,INTERFACE_COMPILE_OPTIONS>
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/freestanding-cxx-hdrs-gnu/README")
    add_subdirectory(freestanding-cxx-hdrs-gnu-wrapper)
else ()
    message(FATAL_ERROR "Third-Party: freestanding-cxx-hdrs-gnu submodule not found.")
endif ()

set(LIMINE_DIR "${LIMINE_DIR}" PARENT_SCOPE)
set(OVMF_DIR "${OVMF_DIR}" PARENT_SCOPE)
