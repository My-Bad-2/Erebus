add_library(freestanding-cxx-hdrs INTERFACE)

target_include_directories(freestanding-cxx-hdrs INTERFACE
		"${CMAKE_CURRENT_SOURCE_DIR}/../freestanding-cxx-hdrs-gnu/x86_64/include"
)

target_precompile_headers(freestanding-cxx-hdrs INTERFACE
		"$<$<COMPILE_LANGUAGE:CXX>:<type_traits>>"
		"$<$<COMPILE_LANGUAGE:CXX>:<utility>>"
		"$<$<COMPILE_LANGUAGE:CXX>:<limits>>"
		"$<$<COMPILE_LANGUAGE:CXX>:<algorithm>>"
)
