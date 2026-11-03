# Usage: cmake -P run_quietly.cmake -- <command> [<argument>...]
#
# Runs the command and shows what it printed only if it fails, so that a build that works stays
# quiet. Arguments may not contain semicolons.

set(command "")
set(after_separator FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach (index RANGE ${last})
  if (after_separator)
    list(APPEND command "${CMAKE_ARGV${index}}")
  elseif (CMAKE_ARGV${index} STREQUAL "--")
    set(after_separator TRUE)
  endif ()
endforeach ()

if (NOT command)
  message(FATAL_ERROR "run_quietly.cmake: no command given after --")
endif ()

execute_process(
    COMMAND ${command}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)

if (NOT result EQUAL 0)
  list(GET command 0 program)
  message("${output}")
  message(FATAL_ERROR "${program} failed (${result})")
endif ()
