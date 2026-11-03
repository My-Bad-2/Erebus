# ==============================================================================
# Module: QemuEmulator
# Description: Targets that boot an ISO in QEMU with OVMF.
#
#   add_qemu_targets(<prefix> ...), for the kernel:
#     <prefix>_run        boots it, serial console on the terminal
#     <prefix>_headless   the same without a display window
#     <prefix>_debug      frozen at the first instruction, GDB stub on :1234
#
#   add_kernel_test_targets(...), for the test kernel:
#     check               boots it once and fails unless every test passed;
#                         also registered with CTest as kernel_tests
#     qemu_stress         boots it again and again across a matrix of machine
#                         configurations and reports which ones fail
#
# The EREBUS_QEMU_* cache variables set up the machine; EREBUS_QEMU_STRESS_*
# the stress matrix. Lists are CMake lists, e.g. -DEREBUS_QEMU_STRESS_SMP="1;4;16".
# ==============================================================================

include_guard(GLOBAL)

find_program(QEMU_PROGRAM qemu-system-x86_64)
find_package(Python3 3.8 COMPONENTS Interpreter QUIET)

if (QEMU_PROGRAM)
  message(STATUS "Emulator: Found QEMU -> ${QEMU_PROGRAM}")
else ()
  message(STATUS "Emulator: qemu-system-x86_64 not found; the QEMU targets will fail if built.")
endif ()

# ------------------------------------------------------------------------------
# The machine
# ------------------------------------------------------------------------------
set(EREBUS_QEMU_MEMORY "2G" CACHE STRING "Guest memory for the QEMU targets")
set(EREBUS_QEMU_SMP "4" CACHE STRING "Guest CPUs for the QEMU targets")
set(EREBUS_QEMU_CPU "max,migratable=no" CACHE STRING "Guest CPU model and feature flags")
set(EREBUS_QEMU_ACCEL "auto" CACHE STRING "QEMU accelerator; auto picks the host's hypervisor")
set_property(CACHE EREBUS_QEMU_ACCEL PROPERTY STRINGS auto kvm hvf whpx tcg)
set(EREBUS_QEMU_EXTRA_ARGS "" CACHE STRING "Extra QEMU arguments for every target (a list)")

# ------------------------------------------------------------------------------
# The stress matrix: every combination, EREBUS_QEMU_STRESS_ITERATIONS times.
# ------------------------------------------------------------------------------
set(EREBUS_QEMU_STRESS_ITERATIONS "2" CACHE STRING "Boots per configuration")
set(EREBUS_QEMU_STRESS_JOBS "1" CACHE STRING
    "Boots at a time; more than the host has CPUs for makes the guests look slow")
set(EREBUS_QEMU_STRESS_TIMEOUT "120" CACHE STRING "Seconds a boot may take")
set(EREBUS_QEMU_STRESS_ACCELERATORS "" CACHE STRING
    "Accelerators to test (empty: the one EREBUS_QEMU_ACCEL picks); tcg is slow but strict")
set(EREBUS_QEMU_STRESS_SMP "1;2;4;8" CACHE STRING "Guest CPU counts")
set(EREBUS_QEMU_STRESS_MEMORY "512M;2G" CACHE STRING "Guest memory sizes")
set(EREBUS_QEMU_STRESS_NUMA_NODES "1;2" CACHE STRING
    "NUMA node counts; memory and CPUs are split evenly between the nodes")
set(EREBUS_QEMU_STRESS_CPU_MODELS
    "max,migratable=no;max,migratable=no,-x2apic;max,migratable=no,-pcid,-invpcid,-fsgsbase;max,migratable=no,-rdseed,-rdrand"
    CACHE STRING "Guest CPU models: everything the host has, then without x2APIC, without the TLB and GS base extensions, and without a hardware RNG")
set(EREBUS_QEMU_STRESS_EXPECT "" CACHE STRING
    "Extra regexes the console must show for a boot to pass")
set(EREBUS_QEMU_STRESS_FAIL "\\[FTL\\]" CACHE STRING
    "Regexes that fail a boot as soon as the console shows them")

function(_erebus_qemu_accel out)
  set(accel "${EREBUS_QEMU_ACCEL}")
  if (accel STREQUAL "auto")
    if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/dev/kvm")
      set(accel kvm)
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
      set(accel hvf)
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
      set(accel whpx)
    else ()
      set(accel tcg)
    endif ()
  endif ()

  set(${out} "${accel}" PARENT_SCOPE)
endfunction()

# A target that fails, saying why, for when a tool is missing.
function(_erebus_unavailable target message)
  add_custom_target(${target}
      COMMAND "${CMAKE_COMMAND}" -E echo "${message}"
      COMMAND "${CMAKE_COMMAND}" -E false
      VERBATIM
  )
endfunction()

# ==============================================================================
# Function: add_qemu_targets
# Usage:    add_qemu_targets(<prefix>
#                            ISO_TARGET <target> ISO_FILE <file>
#                            OVMF_CODE <file> OVMF_VARS <file>)
# ==============================================================================
function(add_qemu_targets prefix)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ISO_FILE;ISO_TARGET;OVMF_CODE;OVMF_VARS" "")

  if (NOT ARG_ISO_FILE OR NOT ARG_ISO_TARGET OR NOT ARG_OVMF_CODE OR NOT ARG_OVMF_VARS)
    message(FATAL_ERROR
        "add_qemu_targets(${prefix}): ISO_FILE, ISO_TARGET, OVMF_CODE and OVMF_VARS are required.")
  endif ()

  if (NOT QEMU_PROGRAM)
    foreach (kind run headless debug)
      _erebus_unavailable(${prefix}_${kind} "Cannot run QEMU: install qemu-system-x86_64.")
    endforeach ()
    return()
  endif ()

  _erebus_qemu_accel(accel)

  # The firmware writes its variables back, so it gets a copy of its own.
  set(vars "${CMAKE_CURRENT_BINARY_DIR}/${prefix}_OVMF_VARS.fd")
  add_custom_command(
      OUTPUT "${vars}"
      COMMAND "${CMAKE_COMMAND}" -E copy "${ARG_OVMF_VARS}" "${vars}"
      DEPENDS "${ARG_OVMF_VARS}"
      COMMENT "Staging writable UEFI variables"
      VERBATIM
  )
  add_custom_target(${prefix}_vars DEPENDS "${vars}")

  set(machine
      -M q35,spcr=on
      -m ${EREBUS_QEMU_MEMORY}
      -smp ${EREBUS_QEMU_SMP}
      -cpu ${EREBUS_QEMU_CPU}
      # The host's hypervisor if it can, plain emulation if not.
      -accel ${accel}
      -accel tcg
      -cdrom "${ARG_ISO_FILE}"
      -drive "if=pflash,format=raw,readonly=on,file=${ARG_OVMF_CODE}"
      -drive "if=pflash,format=raw,readonly=off,file=${vars}"
      # Stop on a triple fault instead of rebooting, and keep the monitor afterwards.
      -no-reboot
      -no-shutdown
      ${EREBUS_QEMU_EXTRA_ARGS}
  )

  add_custom_target(${prefix}_run
      COMMAND "${QEMU_PROGRAM}" ${machine} -serial stdio -d guest_errors
      DEPENDS ${ARG_ISO_TARGET} ${prefix}_vars
      COMMENT "Booting ${ARG_ISO_FILE} in QEMU"
      USES_TERMINAL
      VERBATIM
  )

  add_custom_target(${prefix}_headless
      COMMAND "${QEMU_PROGRAM}" ${machine} -display none -serial stdio
      DEPENDS ${ARG_ISO_TARGET} ${prefix}_vars
      COMMENT "Booting ${ARG_ISO_FILE} in QEMU without a display (Ctrl-A X quits)"
      USES_TERMINAL
      VERBATIM
  )

  # -s: GDB stub on localhost:1234. -S: wait for the debugger before the first instruction.
  add_custom_target(${prefix}_debug
      COMMAND "${QEMU_PROGRAM}" ${machine} -serial stdio -s -S -d int -D qemu.log -M smm=off
      DEPENDS ${ARG_ISO_TARGET} ${prefix}_vars
      COMMENT "Booting ${ARG_ISO_FILE} frozen; attach a debugger to localhost:1234"
      USES_TERMINAL
      VERBATIM
  )
endfunction()

# ==============================================================================
# Function: add_kernel_test_targets
# Usage:    add_kernel_test_targets(ISO_TARGET <target> ISO_FILE <file>
#                                   OVMF_CODE <file> OVMF_VARS <file>)
#
# The test kernel reports its result through QEMU's isa-debug-exit device:
# QEMU exits with 1 when every test passed and with 3 when one failed.
# ==============================================================================
function(add_kernel_test_targets)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "ISO_FILE;ISO_TARGET;OVMF_CODE;OVMF_VARS" "")

  if (NOT ARG_ISO_FILE OR NOT ARG_ISO_TARGET OR NOT ARG_OVMF_CODE OR NOT ARG_OVMF_VARS)
    message(FATAL_ERROR
        "add_kernel_test_targets: ISO_FILE, ISO_TARGET, OVMF_CODE and OVMF_VARS are required.")
  endif ()

  if (NOT QEMU_PROGRAM)
    _erebus_unavailable(check "Cannot run the kernel tests: install qemu-system-x86_64.")
    _erebus_unavailable(qemu_stress "Cannot stress-test: install qemu-system-x86_64.")
    return()
  endif ()

  _erebus_qemu_accel(accel)

  set(debug_exit "")
  if (EREBUS_KTEST_EXIT_PORT)
    set(debug_exit -device "isa-debug-exit,iobase=${EREBUS_KTEST_EXIT_PORT},iosize=0x04")
  endif ()

  if (NOT Python3_Interpreter_FOUND)
    _erebus_unavailable(check "Cannot run the kernel tests: install Python 3.")
    _erebus_unavailable(qemu_stress "Cannot stress-test: install Python 3.")
    return()
  endif ()

  # A boot passes once every CPU is online and every test has passed, and fails the moment a
  # test run or the kernel itself reports failure.
  set(runner "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/scripts/run_kernel_tests.py")
  set(common_args
      --qemu "${QEMU_PROGRAM}"
      --iso "${ARG_ISO_FILE}"
      --ovmf-code "${ARG_OVMF_CODE}"
      --ovmf-vars "${ARG_OVMF_VARS}"
      --timeout ${EREBUS_QEMU_STRESS_TIMEOUT}
  )

  set(expect "SMP: ([0-9]+) of \\1 CPUs online" "ktest: ALL PASSED" ${EREBUS_QEMU_STRESS_EXPECT})
  set(fail "ktest: FAILED" ${EREBUS_QEMU_STRESS_FAIL})
  foreach (pattern IN LISTS expect)
    list(APPEND common_args "--expect=${pattern}")
  endforeach ()
  foreach (pattern IN LISTS fail)
    list(APPEND common_args "--fail=${pattern}")
  endforeach ()
  foreach (arg IN LISTS debug_exit EREBUS_QEMU_EXTRA_ARGS)
    list(APPEND common_args "--extra-arg=${arg}")
  endforeach ()

  # ----------------------------------------------------------------------------
  # check: one boot on the machine the other targets use, console on the terminal
  # ----------------------------------------------------------------------------
  set(check_args
      ${common_args}
      --log-dir "${CMAKE_CURRENT_BINARY_DIR}/check"
      "--accel=${accel}"
      "--cpu=${EREBUS_QEMU_CPU}"
      "--smp=${EREBUS_QEMU_SMP}"
      "--memory=${EREBUS_QEMU_MEMORY}"
      --echo
  )

  add_custom_target(check
      COMMAND ${runner} ${check_args}
      DEPENDS ${ARG_ISO_TARGET}
      COMMENT "Running the kernel tests in QEMU"
      USES_TERMINAL
      VERBATIM
  )

  add_test(NAME kernel_tests COMMAND ${runner} ${check_args})
  set_tests_properties(kernel_tests PROPERTIES TIMEOUT ${EREBUS_QEMU_STRESS_TIMEOUT})

  # ----------------------------------------------------------------------------
  # qemu_stress
  # ----------------------------------------------------------------------------
  set(stress_accels "${EREBUS_QEMU_STRESS_ACCELERATORS}")
  if (NOT stress_accels)
    set(stress_accels "${accel}")
  endif ()

  set(stress_args
      ${common_args}
      --log-dir "${CMAKE_CURRENT_BINARY_DIR}/qemu_stress"
      --iterations ${EREBUS_QEMU_STRESS_ITERATIONS}
      --jobs ${EREBUS_QEMU_STRESS_JOBS}
  )

  # One option per value: CPU models contain commas, and CMake lists semicolons.
  foreach (option_and_list
           "accel:stress_accels" "cpu:EREBUS_QEMU_STRESS_CPU_MODELS" "smp:EREBUS_QEMU_STRESS_SMP"
           "memory:EREBUS_QEMU_STRESS_MEMORY" "numa:EREBUS_QEMU_STRESS_NUMA_NODES")
    string(REPLACE ":" ";" pair "${option_and_list}")
    list(GET pair 0 option)
    list(GET pair 1 values)
    foreach (value IN LISTS ${values})
      list(APPEND stress_args "--${option}=${value}")
    endforeach ()
  endforeach ()

  add_custom_target(qemu_stress
      COMMAND ${runner} ${stress_args}
      DEPENDS ${ARG_ISO_TARGET}
      COMMENT "Stress-testing ${ARG_ISO_FILE} in QEMU"
      USES_TERMINAL
      VERBATIM
  )
endfunction()
