# ======================================================================
# Module: CompilerFlags
# Description: CMake interface for LLVM/Clang and Clang Assembler
# ======================================================================

if (NOT CMAKE_C_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
	message(FATAL_ERROR "CompilerFlags is designed for LLVM toolchain. Current Compiler: ${CMAKE_C_COMPILER_ID}")
endif ()

enable_language(ASM)

#find_program(CCACHE_PROGRAM ccache)
#
#if (CCACHE_PROGRAM)
#	message(STATUS "Found ccache: ${CCACHE_PROGRAM}")
#	set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
#	set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
#else ()
#	message(STATUS "ccache not found - continuing without compiler cache")
#endif ()

set(OS_ARCH_LEVEL "x86-64-v2" CACHE STRING "Target x86-64 microarchitecture level")
set_property(CACHE OS_ARCH_LEVEL PROPERTY STRINGS "x86-64-v2" "x86-64-v3" "x86-64-v4")

option(OS_ENABLE_UBSAN "Enable trapping Undefined Behavior Sanitizer" OFF)

add_library(os_base_flags INTERFACE)

target_compile_options(os_base_flags INTERFACE "-march=${OS_ARCH_LEVEL}"
		-ffreestanding
		-fno-builtin
		-nostdlibinc

		--target=x86_64-elf

		"$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
		"$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
		"$<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>"
		"$<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>"

		"$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>"
		"$<$<COMPILE_LANGUAGE:C>:-std=gnu23>"
		-fno-strict-aliasing

		-Wall -Wextra
		-Werror=return-type
		-Wno-error=pass-failed
		"$<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>"
		-Wno-unused-parameter
		-Wno-missing-field-initializers
		-Wno-gnu
		-fcolor-diagnostics
		-ftemplate-backtrace-limit=0

		"$<$<BOOL:${OS_ENABLE_UBSAN}>:-fsanitize=undefined;-fsanitize-trap=undefined>"

		"$<$<CONFIG:Debug>:-O0;-g3;-fno-omit-frame-pointer;-fno-optimize-sibling-calls;-fno-limit-debug-info>"
		"$<$<CONFIG:Release>:-O3;-DNDEBUG;-flto=thin>"
		"$<$<CONFIG:MinSizeRel>:-Oz;-DNDEBUG;-ffunction-sections;-fdata-sections>"
		"$<$<CONFIG:RelWithDebInfo>:-O2;-g;-DNDEBUG;-fno-omit-frame-pointer;-gsplit-dwarf>"
)

target_link_options(os_base_flags INTERFACE
		-fuse-ld=lld
		-nostdlib
		-Wl,--color-diagnostics
		-Wl,--build-id=sha1
		-Wl,--undefined-version
		-Wl,--fatal-warnings
		"$<$<CONFIG:Debug>:-Wl,--gdb-index>"
		"$<$<CONFIG:Release>:-flto=thin;-Wl,--icf=all>"
		"$<$<CONFIG:MinSizeRel>:-Wl,--gc-sections;-Wl,-s>"
)

# Kernel flags
add_library(os_kernel_flags INTERFACE)

target_link_libraries(os_kernel_flags INTERFACE os_base_flags)
target_compile_options(os_kernel_flags INTERFACE
		-mcmodel=kernel
		-mno-red-zone
		-mgeneral-regs-only
		-mno-80387
)

target_link_options(os_kernel_flags INTERFACE
		-Wl,-znoexecstack
		-Wl,-zmax-page-size=0x1000
		-Wl,-static
)


option(ENABLE_KERNEL_UBSAN "Compile kernel with Undefined Behavior Sanitizer" OFF)

if (ENABLE_KERNEL_UBSAN)
	target_compile_options(os_kernel_flags INTERFACE -fsanitize=undefined)
	target_link_options(os_kernel_flags INTERFACE -fsanitize=undefined)

	message(STATUS "Sanitizers: Kernel UBSan enabled. Ensure __ubsan_* hooks are implemented.")
endif ()
