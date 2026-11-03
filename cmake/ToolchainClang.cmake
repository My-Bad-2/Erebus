# ==============================================================================
# Toolchain: LLVM for a bare-metal x86-64 kernel
#
# cmake -S . -B build --toolchain cmake/ToolchainClang.cmake
#
# Set LLVM_PREFIX to use an LLVM installation other than the one on PATH.
# ==============================================================================

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_PREFIX "" CACHE PATH "Root of the LLVM installation to use (empty: search PATH)")

set(_llvm_hints "")
if (LLVM_PREFIX)
  set(_llvm_hints HINTS "${LLVM_PREFIX}/bin" NO_DEFAULT_PATH)
endif ()

find_program(CMAKE_C_COMPILER NAMES clang ${_llvm_hints} REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ ${_llvm_hints} REQUIRED)
find_program(CMAKE_ASM_COMPILER NAMES clang ${_llvm_hints} REQUIRED)
find_program(CMAKE_AR NAMES llvm-ar ${_llvm_hints} REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib ${_llvm_hints} REQUIRED)
find_program(CMAKE_LINKER NAMES ld.lld ${_llvm_hints} REQUIRED)
find_program(CMAKE_OBJCOPY NAMES llvm-objcopy ${_llvm_hints} REQUIRED)
find_program(CMAKE_OBJDUMP NAMES llvm-objdump ${_llvm_hints} REQUIRED)
find_program(CMAKE_STRIP NAMES llvm-strip ${_llvm_hints} REQUIRED)

# There is no C library to link a test program against: check the compilers by building a
# static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
