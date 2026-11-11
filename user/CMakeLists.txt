# ==============================================================================
# The userland image
#
# Everything but main.cpp is compiled once, into user_objects. The user
# links those with main.cpp; the test user in tests/ links the same objects
# with its own build of main.cpp and the tests.
# ==============================================================================

# ------------------------------------------------------------------------------
# build_info.hpp: name, version and the commit it was configured from
# ------------------------------------------------------------------------------
find_package(Git QUIET)
set(BUILD_INFO_GIT_COMMIT "unknown")
if (GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_VARIABLE BUILD_INFO_GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
endif ()

string(TOLOWER "${PROJECT_NAME}" BUILD_INFO_NAMESPACE)
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/include/generated")
configure_file("${PROJECT_SOURCE_DIR}/cmake/templates/build_info.hpp.in"
        "${generated_dir}/build_info.hpp" @ONLY)

# ------------------------------------------------------------------------------
# user_objects
# ------------------------------------------------------------------------------
set(USER_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

file(GLOB_RECURSE user_sources CONFIGURE_DEPENDS "src/*.cpp" "src/*.c" "src/*.S")
file(GLOB_RECURSE user_headers CONFIGURE_DEPENDS "include/*.h" "include/*.hpp")
list(REMOVE_ITEM user_sources "${USER_MAIN}")

add_library(user_objects OBJECT)
target_sources(user_objects
        PRIVATE ${user_sources}
        PUBLIC FILE_SET HEADERS BASE_DIRS include FILES ${user_headers}
        PUBLIC FILE_SET generated TYPE HEADERS BASE_DIRS "${generated_dir}"
        FILES "${generated_dir}/build_info.hpp"
)

# The C++ headers come before ulib's: libstdc++'s <cstdlib> and friends reach ulib's C headers
# through #include_next, which only searches the directories after their own.
target_link_libraries(user_objects PUBLIC
        erebus::user_abi
        erebus::warnings
        erebus::cxx_headers
        erebus::ulib
        user_lgrt
)

erebus_code_quality(user_objects)

# ==============================================================================
# Function: add_user_image
# Usage:    add_user_image(<target> <sources...>)
#
# A user ELF: user_objects, the given sources (a main.cpp among
# them) and the user's linker script.
# ==============================================================================
function(add_user_image target)
    set(linker_script "${PROJECT_SOURCE_DIR}/user/linker/linker.ld")

    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE user_objects)
    target_link_options(${target} PRIVATE "LINKER:--script=${linker_script}")
    set_target_properties(${target} PROPERTIES LINK_DEPENDS "${linker_script}")
endfunction()

add_user_image(user "${USER_MAIN}")
erebus_code_quality(user)
