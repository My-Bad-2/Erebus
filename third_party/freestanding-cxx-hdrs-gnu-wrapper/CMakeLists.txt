# The freestanding subset of libstdc++'s headers. System headers, so that their own pragmas and
# warnings stay out of the build output.

add_library(freestanding-cxx-hdrs INTERFACE)
add_library(erebus::cxx_headers ALIAS freestanding-cxx-hdrs)

target_include_directories(freestanding-cxx-hdrs SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/../freestanding-cxx-hdrs-gnu/x86_64/include"
)

target_precompile_headers(freestanding-cxx-hdrs INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:<type_traits$<ANGLE-R>>"
    "$<$<COMPILE_LANGUAGE:CXX>:<utility$<ANGLE-R>>"
    "$<$<COMPILE_LANGUAGE:CXX>:<limits$<ANGLE-R>>"
    "$<$<COMPILE_LANGUAGE:CXX>:<algorithm$<ANGLE-R>>"
)
