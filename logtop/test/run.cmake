#
# Fixture tests for logtop, run by ctest or directly:
#
#   cmake -DLOGTOP=<binary> -DFIXTURES=<dir> -DWORKDIR=<dir> -P test/run.cmake
#
# Everything goes through --report, which prints one summary with no timing in
# it, so the expectations can be exact. The last test does drive the live mode,
# but only asserts that it terminates and that the summary is in there.
#

foreach(required LOGTOP FIXTURES WORKDIR)
   if(NOT DEFINED ${required})
      message(FATAL_ERROR "${required} is not defined")
   endif()
endforeach()

# A cross build's binary cannot be run by this host, so it is run through an
# emulator when there is one. Unquoted from here on: LOGTOP may be two words.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/cross-emulator.cmake")
demo_program( LOGTOP)


function(check NAME EXPECTED_STDOUT EXPECTED_RC)
   execute_process(COMMAND ${LOGTOP} ${ARGN}
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
      message("FAIL ${NAME}\n  command: logtop ${ARGN}\n${why}\n  stderr:  [${err}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


# --- the report ------------------------------------------------------------

# field 7 is the status, field 9 the latency: 200 four times, 404 and 500 twice
check("status counts with a latency histogram"
      "logtop report: 8 lines, 0 dropped\nkeys (field 7, top 3 of 3)\n50.0%         4  200\n25.0%         2  404\n25.0%         2  500\nhistogram (field 9, 10 ms buckets, 8 values)\n0-10ms            4  50.0%  ##########\n20-30ms           1  12.5%  ##\n30-40ms           1  12.5%  ##\n40-50ms           1  12.5%  ##\n50-60ms           1  12.5%  ##\n" "0"
      --report -k 7 -V 9 access.log)

# equal counts are ordered by key, so this is stable
check("path counts, ties ordered by key"
      "logtop report: 8 lines, 0 dropped\nkeys (field 5, top 4 of 4)\n62.5%         5  /index.html\n12.5%         1  /missing\n12.5%         1  /submit\n12.5%         1  /x\n" "0"
      --report -k 5 access.log)

check("bucket width is configurable"
      "logtop report: 8 lines, 0 dropped\nkeys (field 7, top 1 of 3)\n50.0%         4  200\nhistogram (field 9, 20 ms buckets, 8 values)\n0-20ms            4  50.0%  ##########\n20-40ms           2  25.0%  #####\n40-60ms           2  25.0%  #####\n" "0"
      --report -k 7 -V 9 --bucket-ms 20 -t 1 access.log)

check("non-numeric value field counts nothing"
      "logtop report: 8 lines, 0 dropped\nkeys (field 7, top 3 of 3)\n50.0%         4  200\n25.0%         2  404\n25.0%         2  500\nhistogram (field 4, 10 ms buckets, 0 values)\n" "0"
      --report -k 7 -V 4 access.log)

# --- options ---------------------------------------------------------------

# -n claims line numbers, so it stops after exactly four lines
check("--lines stops exactly"
      "logtop report: 4 lines, 0 dropped\nkeys (field 7, top 2 of 2)\n75.0%         3  200\n25.0%         1  404\n" "0"
      --report -k 7 -n 4 access.log)

# field 0 is the whole line, and two of the eight lines are identical
check("field 0 tallies whole lines"
      "logtop report: 8 lines, 0 dropped\nkeys (field 0, top 1 of 7)\n25.0%         2  10.0.0.1 - - \"GET /index.html HTTP/1.1\" 200 12 5\n" "0"
      --report -k 0 -t 1 access.log)

check("empty input exits 1"
      "logtop report: 0 lines, 0 dropped\nkeys (field 7, top 0 of 0)\n" "1"
      --report -k 7 empty.log)

# --- thread invariance -----------------------------------------------------

# the tallies are per key, not per parser, so the answer cannot depend on -j
execute_process(COMMAND ${LOGTOP} --report -j1 -k 7 -V 9 access.log
                WORKING_DIRECTORY "${FIXTURES}"
                OUTPUT_VARIABLE one
                RESULT_VARIABLE rc_one)
execute_process(COMMAND ${LOGTOP} --report -j8 -k 7 -V 9 access.log
                WORKING_DIRECTORY "${FIXTURES}"
                OUTPUT_VARIABLE eight
                RESULT_VARIABLE rc_eight)

if(one STREQUAL eight AND "${rc_one}" STREQUAL "0" AND "${rc_eight}" STREQUAL "0")
   message(STATUS "ok   one parser and eight parsers agree")
else()
   message("FAIL one parser and eight parsers agree\n  -j1:\n${one}\n  -j8:\n${eight}")
   set_property(GLOBAL APPEND PROPERTY failures "thread invariance")
endif()

# --- stdin as the source ---------------------------------------------------

file(WRITE "${WORKDIR}/log.txt" "10.0.0.1 - - \"GET /a HTTP/1.1\" 200 1 5\n10.0.0.2 - - \"GET /b HTTP/1.1\" 404 1 7\n")

execute_process(COMMAND ${LOGTOP} --report -k 7
                WORKING_DIRECTORY "${FIXTURES}"
                INPUT_FILE "${WORKDIR}/log.txt"
                OUTPUT_VARIABLE out
                ERROR_VARIABLE  err
                RESULT_VARIABLE rc)

if(out STREQUAL "logtop report: 2 lines, 0 dropped\nkeys (field 7, top 2 of 2)\n50.0%         1  200\n50.0%         1  404\n" AND "${rc}" STREQUAL "0")
   message(STATUS "ok   reads the log from stdin")
else()
   message("FAIL reads the log from stdin\n  stdout: [${out}]\n  exit:   ${rc}\n  stderr: [${err}]")
   set_property(GLOBAL APPEND PROPERTY failures "stdin source")
endif()

# --- live mode -------------------------------------------------------------

# The dashboard is timing dependent, so assert what is stable: it draws, it
# stops after --lines, and the closing summary is the same report as above.
execute_process(COMMAND ${LOGTOP} --no-tui -i 0.05 -n 8 -k 5 access.log
                WORKING_DIRECTORY "${FIXTURES}"
                OUTPUT_VARIABLE live
                ERROR_VARIABLE  err
                RESULT_VARIABLE rc
                TIMEOUT 30)

string(FIND "${live}" "throughput (" has_dashboard)
string(FIND "${live}" "keys (field 5, top 4 of 4)\n62.5%         5  /index.html\n12.5%         1  /missing\n12.5%         1  /submit\n12.5%         1  /x\n" has_summary)

if(NOT has_dashboard EQUAL -1 AND NOT has_summary EQUAL -1 AND "${rc}" STREQUAL "0")
   message(STATUS "ok   live mode draws and then reports")
else()
   message("FAIL live mode draws and then reports\n  dashboard at ${has_dashboard}, summary at ${has_summary}, exit ${rc}\n  output:\n${live}\n  stderr: [${err}]")
   set_property(GLOBAL APPEND PROPERTY failures "live mode")
endif()


get_property(failed GLOBAL PROPERTY failures)
if(failed)
   list(LENGTH failed n)
   message(FATAL_ERROR "${n} logtop test(s) failed: ${failed}")
endif()

message(STATUS "all logtop tests passed")
