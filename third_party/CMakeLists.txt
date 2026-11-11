# ==============================================================================
# Third-party code
#
# Git submodules, built from source: the freestanding C++ headers, uACPI, Zydis
# and a freestanding compiler-rt. Prebuilt downloads: the Limine bootloader and
# OVMF firmware.
#
# Everything that ends up in the kernel is built with the kernel's ABI (no red
# zone, no SSE): code that broke it would corrupt the stack or registers of
# whatever an interrupt arrived in. Warnings in this code are silenced, and its
# include directories are system ones, so it does not drown out Erebus's own.
# ==============================================================================

# ------------------------------------------------------------------------------
# Submodules: fetched when some are missing, never touched otherwise.
# ------------------------------------------------------------------------------
option(EREBUS_FETCH_SUBMODULES "Check out missing git submodules at configure time" ON)

set(missing "")
foreach (module freestanding-cxx-hdrs-gnu uacpi zydis freestanding-lgrt)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${module}/.git")
        list(APPEND missing "${module}")
    endif ()
endforeach ()

if (missing)
    find_package(Git QUIET)
    if (NOT EREBUS_FETCH_SUBMODULES OR NOT GIT_FOUND OR NOT EXISTS "${PROJECT_SOURCE_DIR}/.git")
        message(FATAL_ERROR "Third-party: missing submodules (${missing}). "
                "Run: git submodule update --init --recursive")
    endif ()

    message(STATUS "Third-party: checking out ${missing}")
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" submodule update --init --recursive --depth 1
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error
    )
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "Third-party: git submodule update failed:\n${error}")
    endif ()
endif ()

# ------------------------------------------------------------------------------
# Prebuilt downloads
# ------------------------------------------------------------------------------
add_subdirectory(limine)
add_subdirectory(ovmf)

set(LIMINE_DIR "${LIMINE_DIR}" PARENT_SCOPE)
set(OVMF_DIR "${OVMF_DIR}" PARENT_SCOPE)

# ------------------------------------------------------------------------------
# Sources
# ------------------------------------------------------------------------------
add_subdirectory(freestanding-cxx-hdrs-gnu-wrapper)
add_subdirectory(uacpi-wrapper)

# Compiler-rt's builtins: 128-bit division, bit counting, atomics wider than 8 bytes.
add_library(lgrt STATIC "${CMAKE_CURRENT_SOURCE_DIR}/freestanding-lgrt/src/freestanding-lgrt.c")
target_link_libraries(lgrt PRIVATE erebus::kernel_abi erebus::third_party)

add_library(user_lgrt STATIC "${CMAKE_CURRENT_SOURCE_DIR}/freestanding-lgrt/src/freestanding-lgrt.c")
target_link_libraries(user_lgrt PRIVATE erebus::user_abi erebus::third_party)

# Zydis: an x86 decoder and encoder. Only what a kernel can use. Its option() calls predate
# CMP0077, under which they let these variables override their defaults.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(ZYDIS_FEATURE_FORMATTER OFF)
set(ZYDIS_BUILD_SHARED_LIB OFF)
set(ZYDIS_BUILD_EXAMPLES OFF)
set(ZYDIS_BUILD_TOOLS OFF)
set(ZYDIS_BUILD_TESTS OFF)
set(ZYDIS_BUILD_DOXYGEN OFF)
set(ZYDIS_BUILD_MAN OFF)
set(ZYAN_NO_LIBC ON)
set(ZYAN_WHOLE_PROGRAM_OPTIMIZATION ON)
add_subdirectory(zydis EXCLUDE_FROM_ALL SYSTEM)

# Zydis exports its targets for installation; BUILD_LOCAL_INTERFACE keeps our flag targets out
# of that export.
foreach (target Zydis Zycore)
    target_link_libraries(${target} PRIVATE
            "$<BUILD_LOCAL_INTERFACE:erebus::kernel_abi;erebus::third_party>")
endforeach ()
