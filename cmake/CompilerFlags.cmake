# ==============================================================================
# Module: CompilerFlags
# Description: Compile and link settings, as interface targets to link against:
#
#   erebus::freestanding   x86-64 ELF, freestanding C and C++, no hosted runtime.
#                          Everything that ends up in the kernel image uses it.
#   erebus::kernel_abi     The kernel's code model and register restrictions.
#   erebus::warnings       The warning policy for Erebus's own code: mistakes
#                          that are never intended fail the build, matters of
#                          style stay quiet.
#   erebus::third_party    Silences warnings in code Erebus does not maintain.
#                          Its errors are still errors.
# ==============================================================================

include_guard(GLOBAL)

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Erebus builds with Clang and LLD; this is ${CMAKE_CXX_COMPILER_ID}.")
endif ()

function(_erebus_migrate_option old new type)
  if (DEFINED CACHE{${old}} AND NOT DEFINED CACHE{${new}})
    set(${new} "$CACHE{${old}}" CACHE ${type} "" FORCE)
  endif ()

  unset(${old} CACHE)
endfunction()

_erebus_migrate_option(OS_ARCH_LEVEL EREBUS_ARCH_LEVEL STRING)
_erebus_migrate_option(OS_ENABLE_UBSAN EREBUS_UBSAN BOOL)
unset(ENABLE_KERNEL_UBSAN CACHE)

set(EREBUS_ARCH_LEVEL "x86-64-v2" CACHE STRING "Target x86-64 microarchitecture (-march)")
set_property(CACHE EREBUS_ARCH_LEVEL PROPERTY STRINGS
    x86-64-v2 x86-64-v3 x86-64-v4 znver4 znver5)

option(EREBUS_UBSAN "Trap on undefined behaviour in the kernel (UBSan, trap mode)" OFF)
option(EREBUS_WERROR "Treat every warning in Erebus's own code as an error" OFF)

# ------------------------------------------------------------------------------
# erebus::freestanding
# ------------------------------------------------------------------------------
add_library(erebus_freestanding INTERFACE)
add_library(erebus::freestanding ALIAS erebus_freestanding)

target_compile_options(erebus_freestanding INTERFACE
    --target=x86_64-elf
    -march=${EREBUS_ARCH_LEVEL}
    -ffreestanding
    -fno-builtin
    -fno-strict-aliasing
    -fPIE
    -fcolor-diagnostics
    -ftemplate-backtrace-limit=0

    $<$<COMPILE_LANGUAGE:C>:-std=gnu23>
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++2d>
    $<$<COMPILE_LANGUAGE:CXX>:-nostdlibinc>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>

    "$<$<BOOL:${EREBUS_UBSAN}>:-fsanitize=undefined;-fsanitize-trap=undefined>"

    # On top of CMake's own per-configuration flags (-O3 -DNDEBUG for Release and so on).
    "$<$<CONFIG:Debug>:-Og;-g3;-fno-omit-frame-pointer;-fno-optimize-sibling-calls;-fno-limit-debug-info>"
    "$<$<CONFIG:Release>:-flto=thin;-fomit-frame-pointer>"
    "$<$<CONFIG:MinSizeRel>:-Oz;-flto=thin;-ffunction-sections;-fdata-sections;-fomit-frame-pointer>"
    "$<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>"
)

target_link_options(erebus_freestanding INTERFACE
    -fuse-ld=lld
    -nostdlib
    LINKER:--color-diagnostics
    LINKER:--build-id=sha1
    LINKER:--undefined-version
    LINKER:--fatal-warnings
    "$<$<CONFIG:Debug>:-Wl,--gdb-index>"
    "$<$<CONFIG:Release>:-flto=thin;-Wl,--icf=all>"
    "$<$<CONFIG:MinSizeRel>:-flto=thin;-Wl,--gc-sections;-Wl,-s>"
)

# ------------------------------------------------------------------------------
# erebus::kernel_abi
# ------------------------------------------------------------------------------
add_library(erebus_kernel_abi INTERFACE)
add_library(erebus::kernel_abi ALIAS erebus_kernel_abi)

target_link_libraries(erebus_kernel_abi INTERFACE erebus::freestanding)
target_compile_options(erebus_kernel_abi INTERFACE
    -mcmodel=kernel
    -mno-red-zone
    -mgeneral-regs-only
    -mno-80387
)
target_link_options(erebus_kernel_abi INTERFACE
    LINKER:-z,noexecstack
    LINKER:-z,max-page-size=0x1000
    LINKER:-static
)

# ------------------------------------------------------------------------------
# erebus::warnings
# ------------------------------------------------------------------------------
add_library(erebus_warnings INTERFACE)
add_library(erebus::warnings ALIAS erebus_warnings)

target_compile_options(erebus_warnings INTERFACE
    -Wall
    -Wextra

    # Style, or deliberate in this code base: not worth a word.
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-gnu
    # Optimizer remarks about loops it chose not to unroll.
    -Wno-pass-failed

    # Never intended, whatever the code looks like: always errors.
    -Werror=return-type
    -Werror=uninitialized
    -Werror=sometimes-uninitialized
    -Werror=array-bounds
    -Werror=dangling
    -Werror=dangling-gsl
    -Werror=return-stack-address
    -Werror=division-by-zero
    -Werror=shift-count-overflow
    -Werror=shift-count-negative
    -Werror=null-dereference
    -Werror=nonnull
    -Werror=sizeof-pointer-memaccess
    -Werror=memset-transposed-args
    -Werror=dynamic-class-memaccess
    -Werror=builtin-memcpy-chk-size
    -Werror=unsequenced
    -Werror=infinite-recursion
    -Werror=undefined-inline
    -Werror=undefined-internal
    -Werror=self-move
    -Werror=delete-incomplete
    -Werror=mismatched-new-delete
    -Werror=free-nonheap-object
    -Werror=string-plus-int
    -Werror=format
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:C>:-Werror=int-conversion>
    $<$<COMPILE_LANGUAGE:C>:-Werror=incompatible-pointer-types>

    $<$<BOOL:${EREBUS_WERROR}>:-Werror>
)

# ------------------------------------------------------------------------------
# erebus::third_party
# ------------------------------------------------------------------------------
add_library(erebus_third_party INTERFACE)
add_library(erebus::third_party ALIAS erebus_third_party)

target_compile_options(erebus_third_party INTERFACE -w)
