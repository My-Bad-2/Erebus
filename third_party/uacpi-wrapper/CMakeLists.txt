# uACPI in barebones mode: table access only, no AML interpreter.

set(UACPI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../uacpi")
file(GLOB_RECURSE UACPI_SOURCES CONFIGURE_DEPENDS "${UACPI_ROOT}/source/*.c")

add_library(uacpi STATIC ${UACPI_SOURCES})
add_library(uacpi::uacpi ALIAS uacpi)

target_link_libraries(uacpi PRIVATE erebus::kernel_abi erebus::third_party)
target_include_directories(uacpi SYSTEM PUBLIC "${UACPI_ROOT}/include")
target_compile_definitions(uacpi PUBLIC UACPI_BAREBONES_MODE)
