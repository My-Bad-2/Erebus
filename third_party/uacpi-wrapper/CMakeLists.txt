file(GLOB_RECURSE UACPI_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/../uacpi/source/*.c"
)

add_library(uacpi STATIC ${UACPI_SOURCES})

target_link_libraries(uacpi PRIVATE os_kernel_flags)
target_include_directories(uacpi PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/../uacpi/include"
)

target_compile_definitions(uacpi PUBLIC UACPI_BAREBONES_MODE)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
	target_compile_options(uacpi PRIVATE
			-Wno-unused-parameter
			-Wno-missing-field-initializers
			-Wno-sign-compare
	)
endif()
