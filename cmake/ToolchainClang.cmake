# ==============================================================================
# ToolchainClang.cmake
# ==============================================================================

set(CMAKE_SYSTEM_NAME Generic)

set(LLVM_PREFIX "" CACHE PATH "Path to a custom LLVM installation root (e.g., /opt/llvm)")

if(LLVM_PREFIX)
	set(LLVM_BIN_DIR "${LLVM_PREFIX}/bin/")
else()
	set(LLVM_BIN_DIR "")
endif()

find_program(CMAKE_C_COMPILER   NAMES "${LLVM_BIN_DIR}clang"   REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES "${LLVM_BIN_DIR}clang++" REQUIRED)
find_program(CMAKE_ASM_COMPILER NAMES "${LLVM_BIN_DIR}clang"   REQUIRED)

find_program(CMAKE_AR      NAMES "${LLVM_BIN_DIR}llvm-ar"      REQUIRED)
find_program(CMAKE_RANLIB  NAMES "${LLVM_BIN_DIR}llvm-ranlib"  REQUIRED)
find_program(CMAKE_LINKER  NAMES "${LLVM_BIN_DIR}ld.lld"       REQUIRED)
find_program(CMAKE_OBJCOPY NAMES "${LLVM_BIN_DIR}llvm-objcopy" REQUIRED)
find_program(CMAKE_OBJDUMP NAMES "${LLVM_BIN_DIR}llvm-objdump" REQUIRED)
find_program(CMAKE_STRIP   NAMES "${LLVM_BIN_DIR}llvm-strip"   REQUIRED)

set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)