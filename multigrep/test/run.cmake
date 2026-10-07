#
# Fixture tests for multigrep, run by ctest or directly:
#
#   cmake -DMULTIGREP=<binary> -DFIXTURES=<dir> -DWORKDIR=<dir> -P test/run.cmake
#
# Every command runs with the fixture directory as its working directory so the
# expected output can use short relative names.
#

foreach(required MULTIGREP FIXTURES WORKDIR)
   if(NOT DEFINED ${required})
      message(FATAL_ERROR "${required} is not defined")
   endif()
endforeach()

# A cross build's binary cannot be run by this host, so it is run through an
# emulator when there is one. Unquoted from here on: MULTIGREP may be two words.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/cross-emulator.cmake")
demo_program( MULTIGREP)


function(check NAME EXPECTED_STDOUT EXPECTED_RC)
   execute_process(COMMAND ${MULTIGREP} ${ARGN}
                   WORKING_DIRECTORY "${FIXTURES}"
                   OUTPUT_VARIABLE out
                   ERROR_VARIABLE  err
                   RESULT_VARIABLE rc)

   set(why "")

   if(NOT out STREQUAL "${EXPECTED_STDOUT}")
      set(why "  stdout: expected [${EXPECTED_STDOUT}]\n          got      [${out}]")
   endif()

   if(NOT "${rc}" STREQUAL "${EXPECTED_RC}")
      set(why "${why}\n  exit:   expected ${EXPECTED_RC}, got ${rc}")
   endif()

   if(why)
      message("FAIL ${NAME}\n  command: multigrep ${ARGN}\n${why}\n  stderr:  [${err}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


function(check_stdin NAME PATH_LIST EXPECTED_STDOUT EXPECTED_RC)
   file(WRITE "${WORKDIR}/paths.txt" "${PATH_LIST}")

   execute_process(COMMAND ${MULTIGREP} ${ARGN}
                   WORKING_DIRECTORY "${FIXTURES}"
                   INPUT_FILE "${WORKDIR}/paths.txt"
                   OUTPUT_VARIABLE out
                   ERROR_VARIABLE  err
                   RESULT_VARIABLE rc)

   if(out STREQUAL "${EXPECTED_STDOUT}" AND "${rc}" STREQUAL "${EXPECTED_RC}")
      message(STATUS "ok   ${NAME}")
   else()
      message("FAIL ${NAME}\n  stdin:   [${PATH_LIST}]\n  stdout:  expected [${EXPECTED_STDOUT}]\n           got      [${out}]\n  exit:    expected ${EXPECTED_RC}, got ${rc}\n  stderr:  [${err}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   endif()
endfunction()


# --- per file counts -------------------------------------------------------

check("count, one thread"
      "2\ta.txt\n1\tb.txt\n" "0"
      -j1 -c needle a.txt b.txt)

# the same counts via four threads: the lock-free structures must not change
# the answer, only the order in which it is discovered
check("count, four threads"
      "2\ta.txt\n1\tb.txt\n" "0"
      -j4 -c needle a.txt b.txt)

check("count, case-insensitive"
      "3\ta.txt\n1\tb.txt\n" "0"
      -i -j1 -c needle a.txt b.txt)

# needs mulle-unicode folding, not ASCII folding: the pattern is not ASCII
check("unicode case folding"
      "1\ta.txt\n" "0"
      -i -j4 -c NÄDEL a.txt)

# --- match output ----------------------------------------------------------

check("matches, byte columns"
      "a.txt:2:1:needle here\na.txt:5:6:αα needle\nb.txt:2:1:needle here\n" "0"
      -j1 needle a.txt b.txt)

# the αα prefix is four bytes but three characters
check("matches, character columns"
      "a.txt:2:1:needle here\na.txt:5:4:αα needle\n" "0"
      -j1 --chars needle a.txt)

check("files with matches"
      "a.txt\nb.txt\n" "0"
      -l -j1 needle a.txt b.txt)

# --- unique lines ----------------------------------------------------------

# "needle here" is in both files, so only the first sighting is reported
check("unique, first sighting only"
      "a.txt:2:1:needle here\na.txt:5:6:αα needle\n" "0"
      -u -j1 needle a.txt b.txt)

# with -c the counts are final, so the output is stable across thread counts
check("unique, final counts"
      "2\tneedle here\n1\tαα needle\n" "0"
      -u -c -j4 needle a.txt b.txt)

check("top files by hit count"
      "3\ta.txt\n1\tb.txt\n" "0"
      -T 2 -i needle a.txt b.txt)

# --- edge cases ------------------------------------------------------------

check("no match exits 1"
      "" "1"
      -j1 -c zzz a.txt b.txt)

# -- ends the options, so a pattern starting with - is searchable at all; the
# second line's first occurrence is the one column that gets reported
check("-- ends the options"
      "c.txt:1:1:-dash first\nc.txt:2:3:x -dash -dash\n" "0"
      -j1 -- -dash c.txt)

# and with -- in front, a pattern that needs no escaping still behaves
check("-- with an ordinary pattern"
      "2\ta.txt\n1\tb.txt\n" "0"
      -j1 -c -- needle a.txt b.txt)

# An empty pattern matches every line, like grep. Spelled out instead of going
# through check(), because a function's ${ARGN} drops empty list elements and
# the empty argument would never reach the program.
#
execute_process(COMMAND ${MULTIGREP} -j1 -c "" a.txt b.txt
                WORKING_DIRECTORY "${FIXTURES}"
                OUTPUT_VARIABLE out
                ERROR_VARIABLE  err
                RESULT_VARIABLE rc)

if(out STREQUAL "5\ta.txt\n2\tb.txt\n" AND "${rc}" STREQUAL "0")
   message(STATUS "ok   empty pattern matches every line")
else()
   message("FAIL empty pattern matches every line\n  stdout: [${out}]\n  exit:   ${rc}\n  stderr: [${err}]")
   set_property(GLOBAL APPEND PROPERTY failures "empty pattern matches every line")
endif()

check_stdin("path list on stdin"
            "a.txt\nb.txt\n"
            "2\ta.txt\n1\tb.txt\n" "0"
            -j4 -c needle)


get_property(failed GLOBAL PROPERTY failures)
if(failed)
   list(LENGTH failed n)
   message(FATAL_ERROR "${n} multigrep test(s) failed: ${failed}")
endif()

message(STATUS "all multigrep tests passed")
