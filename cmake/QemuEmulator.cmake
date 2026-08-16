# ==============================================================================
# Module: QemuEmulator
# Description: Provides hardware-accelerated x86-64 QEMU emulation targets
#              with OVMF UEFI support and LLDB remote debugging hooks.
# ==============================================================================

find_program(QEMU_PROGRAM qemu-system-x86_64)

if (QEMU_PROGRAM)
    message(STATUS "Emulator: Found QEMU -> ${QEMU_PROGRAM}")
else ()
    message(WARNING "Emulator: qemu-system-x86_64 not found. Emulation targets will not be available.")
endif ()

# ==============================================================================
# Function: add_qemu_targets
# Arguments:
#   TARGET_PREFIX - Prefix for the generated targets
#   ISO_FILE      - Path to the generated bootable ISO
#   ISO_TARGET    - The CMake target that builds the ISO
#   OVMF_CODE     - Path to the OVMF_CODE.fd file
#   OVMF_VARS     - Path to the OVMF_VARS.fd file
#   MEMORY        - (Optional) RAM size, defaults to "2G"
#   SMP           - (Optional) Number of CPU cores, defaults to "2"
# ==============================================================================
function(add_qemu_targets TARGET_PREFIX)
    cmake_parse_arguments(ARG "" "ISO_FILE;ISO_TARGET;OVMF_CODE;OVMF_VARS;MEMORY;SMP" "" ${ARGN})

    if (NOT ARG_ISO_FILE OR NOT ARG_ISO_TARGET OR NOT ARG_OVMF_CODE OR NOT ARG_OVMF_VARS)
        message(FATAL_ERROR "add_qemu_targets: ISO_FILE, ISO_TARGET, OVMF_CODE, and OVMF_VARS are required.")
    endif ()

    if (NOT QEMU_PROGRAM)
        return()
    endif ()

    if (NOT ARG_MEMORY)
        set(ARG_MEMORY "2G")
    endif ()
    if (NOT ARG_SMP)
        set(ARG_SMP "2")
    endif ()

    # Determine Host Hardware Acceleration
    if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(HOST_ACCEL "whpx")
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(HOST_ACCEL "hvf")
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(HOST_ACCEL "kvm")
    else ()
        set(HOST_ACCEL "tcg")
    endif ()

    # QEMU will refuse to boot if the OVMF_VARS file is strictly read-only.
    # We must duplicate it into the binary directory so the UEFI firmware can write to it.
    set(LOCAL_OVMF_VARS "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PREFIX}_OVMF_VARS.fd")

    add_custom_command(
            OUTPUT "${LOCAL_OVMF_VARS}"
            COMMAND ${CMAKE_COMMAND} -E copy "${ARG_OVMF_VARS}" "${LOCAL_OVMF_VARS}"
            DEPENDS "${ARG_OVMF_VARS}"
            COMMENT "Staging writable UEFI NVRAM variables..."
    )

    # Create a target for the vars file so the run targets can depend on it
    set(VARS_TARGET "${TARGET_PREFIX}_vars")
    add_custom_target(${VARS_TARGET} DEPENDS "${LOCAL_OVMF_VARS}")

    set(QEMU_BASE_ARGS
            -M q35,spcr=on
            -m ${ARG_MEMORY}
            -smp ${ARG_SMP}

            # 'max' exposes all host CPU features to the guest OS
            -cpu max

            # Tries the host hypervisor first, falls back to software rendering (tcg)
            -accel ${HOST_ACCEL}
            -accel tcg

            # Media
            -cdrom "${ARG_ISO_FILE}"

            # UEFI Firmware Mapping
            -drive if=pflash,format=raw,readonly=on,file=${ARG_OVMF_CODE}
            -drive if=pflash,format=raw,readonly=off,file=${LOCAL_OVMF_VARS}

            -serial stdio        # Routes COM1 serial output directly to the host terminal
            -d guest_errors      # Dumps invalid memory accesses and I/O to the console
            -no-reboot           # Halts on triple fault instead of looping endlessly
            -no-shutdown         # Keeps the monitor alive after guest shutdown
    )

    add_custom_target(${TARGET_PREFIX}_run
            COMMAND ${QEMU_PROGRAM} ${QEMU_BASE_ARGS}
            DEPENDS ${ARG_ISO_TARGET} ${VARS_TARGET}
            COMMENT "Booting ISO in QEMU..."
            USES_TERMINAL
    )

    # -s: Shorthand for -gdb tcp::1234
    # -S: Freeze the CPU at startup (Wait for debugger to attach)
    add_custom_target(${TARGET_PREFIX}_debug
            COMMAND ${QEMU_PROGRAM} ${QEMU_BASE_ARGS} -s -S -d int -D qemu.log -M smm=off
            DEPENDS ${ARG_ISO_TARGET} ${VARS_TARGET}
            COMMENT "Booting ISO in QEMU (Frozen). Waiting for LLDB to attach on localhost:1234..."
            USES_TERMINAL
    )
endfunction()
