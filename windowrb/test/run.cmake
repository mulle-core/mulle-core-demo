#
# Fixture tests for windowrb, run by ctest or directly:
#
#   cmake -DWINDOWRB=<binary> -DFIXTURES=<dir> -DWORKDIR=<dir> -P test/run.cmake
#
# What is asserted is structure, not timing: how many windows opened, closed and
# reported, what the statistics of a hand-checkable fixture are, that the two
# backends agree, and that the failure paths fail. The wall-clock line is the
# machine's and is never matched.
#

foreach(required WINDOWRB FIXTURES WORKDIR)
   if(NOT DEFINED ${required})
      message(FATAL_ERROR "${required} is not defined")
   endif()
endforeach()

# A cross build's binary cannot be run by this host, so it is run through an
# emulator when there is one. Unquoted from here on: WINDOWRB may be two words.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/cross-emulator.cmake")
demo_program( WINDOWRB)


function(run NAME)
   execute_process(COMMAND ${WINDOWRB} ${ARGN}
                   WORKING_DIRECTORY "${WORKDIR}"
                   OUTPUT_VARIABLE out
                   ERROR_VARIABLE  err
                   RESULT_VARIABLE rc)
   set(${NAME}_out "${out}" PARENT_SCOPE)
   set(${NAME}_err "${err}" PARENT_SCOPE)
   set(${NAME}_rc  "${rc}"  PARENT_SCOPE)
endfunction()


function(require_match NAME HAYSTACK PATTERN)
   if(NOT "${HAYSTACK}" MATCHES "${PATTERN}")
      message("FAIL ${NAME}: no match for [${PATTERN}]\n  in: [${HAYSTACK}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


function(require_absent NAME HAYSTACK PATTERN)
   if("${HAYSTACK}" MATCHES "${PATTERN}")
      message("FAIL ${NAME}: unexpected match for [${PATTERN}]\n  in: [${HAYSTACK}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


function(require_exit NAME ACTUAL EXPECTED)
   if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
      message("FAIL ${NAME}: exit ${ACTUAL}, expected ${EXPECTED}")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


# A file of the values 1..16, one per line. A window of all sixteen has median
# 8 (index (16-1)/2 = 7 of the sorted values) and, at a threshold of 2, fourteen
# values with |value| > 2 -- so both statistics can be checked by hand.
set(sixteen "${FIXTURES}/sixteen.txt")


# ---------------------------------------------------------------- one window

# --stdin because the fixture is the input; --tail is what makes the report
# reachable in a --quiet run.
execute_process(COMMAND ${WINDOWRB} --stdin --window-size 16 --start-every 16
                        --max-active 1 --threshold 2 --quiet --tail 1
                INPUT_FILE "${sixteen}"
                OUTPUT_VARIABLE single
                ERROR_VARIABLE  single_err
                RESULT_VARIABLE single_rc)

require_exit("one window exits 0" "${single_rc}" "0")
require_match("one window counts" "${single}"
   "Samples processed : 16\nWindows created   : 1\nWindows destroyed : 1\nReports emitted   : 1\nOutliers total    : 14\n")
require_match("one window statistics" "${single}"
   "\\[window 1 finished @ 15\\] median=8\\.00  outliers=14")
require_match("one window slots" "${single}" "Peak concurrent   : 1 of 1 window slots")
require_match("one window node pool" "${single}"
   "Tree node pool    : 16 nodes handed out, 16 per tree at its high-water \\(buckets of 32\\)")
require_match("one window arena" "${single}"
   "Arenas            : 1 created and released, peak 1 page\\(s\\) each, 1 pages in total")

# The same statistics printed inline rather than in the tail block.
execute_process(COMMAND ${WINDOWRB} --stdin --window-size 16 --start-every 16
                        --max-active 1 --threshold 2
                INPUT_FILE "${sixteen}"
                OUTPUT_VARIABLE inline_out
                RESULT_VARIABLE inline_rc)

require_match("report line while it happens" "${inline_out}"
   "\\[window 1 finished @ 15\\] median=8\\.00  outliers=14  \\(lived 16 samples\\)")


# --------------------------------------------------------------- staggered

# window-size 16 with start-every 4 means four windows are open at once, and
# --max-active 8 is therefore not what limits concurrency -- which the summary
# has to say, because otherwise "peak 4 of 8" reads as a bug.
execute_process(COMMAND ${WINDOWRB} --stdin --window-size 16 --start-every 4
                        --max-active 8 --threshold 2 --quiet
                INPUT_FILE "${sixteen}"
                OUTPUT_VARIABLE staggered
                RESULT_VARIABLE staggered_rc)

require_exit("staggered exits 0" "${staggered_rc}" "0")
require_match("staggered counts" "${staggered}"
   "Windows created   : 4\nWindows destroyed : 4\nReports emitted   : 1\n")
require_match("staggered concurrency" "${staggered}" "Peak concurrent   : 4 of 8 window slots")
require_match("staggered binding limit" "${staggered}"
   "--max-active 8 is not the binding limit")


# ------------------------------------------------------- the generated stream

execute_process(COMMAND ${WINDOWRB} --quiet --tail 3
                OUTPUT_VARIABLE generated
                ERROR_VARIABLE  generated_err
                RESULT_VARIABLE generated_rc)

require_exit("generated stream exits 0" "${generated_rc}" "0")
require_match("generated counts" "${generated}"
   "Samples processed : 20000\nWindows created   : 1250\nWindows destroyed : 1250\nReports emitted   : 1247\n")
require_match("generated tail" "${generated}" "Last report\\(s\\):")

# The generator is ours and --seed is the default, so two runs have to produce
# the same reports. Only the tail block is compared: the timing line is not the
# same twice, and asserting it would be a stopwatch.
string(REGEX MATCH "Last report\\(s\\):\n.*" first_tail "${generated}")
execute_process(COMMAND ${WINDOWRB} --quiet --tail 3
                OUTPUT_VARIABLE generated2
                RESULT_VARIABLE generated2_rc)
string(REGEX MATCH "Last report\\(s\\):\n.*" second_tail "${generated2}")

if(first_tail STREQUAL second_tail AND NOT first_tail STREQUAL "")
   message(STATUS "ok   the same seed gives the same reports")
else()
   message("FAIL the same seed gives the same reports\n  1: [${first_tail}]\n  2: [${second_tail}]")
   set_property(GLOBAL APPEND PROPERTY failures "seed determinism")
endif()

# A different seed has to change the data (it need not change the counts).
execute_process(COMMAND ${WINDOWRB} --quiet --tail 3 --seed 7
                OUTPUT_VARIABLE seeded
                RESULT_VARIABLE seeded_rc)
string(REGEX MATCH "Last report\\(s\\):\n.*" seeded_tail "${seeded}")

if(NOT seeded_tail STREQUAL first_tail)
   message(STATUS "ok   another seed gives other reports")
else()
   message("FAIL another seed gives other reports: [${seeded_tail}]")
   set_property(GLOBAL APPEND PROPERTY failures "seed changes data")
endif()


# ----------------------------------------------------------- the two backends

run(verify --baseline --verify --quiet)
require_exit("verify exits 0" "${verify_rc}" "0")
require_match("verify agrees" "${verify_out}"
   "verify: both backends agree on 1247 reports")
require_match("baseline summary" "${verify_out}" "array \\+ mulle_qsort_r per window")
require_match("baseline storage" "${verify_out}"
   "Window values     : 64 doubles per window in the arena")

# Teeth: --perturb makes the baseline's outlier count wrong on purpose, and the
# verification has to notice and fail the run.
run(perturbed --baseline --verify --perturb --quiet)
require_exit("a corrupted baseline is caught" "${perturbed_rc}" "1")
require_match("the mismatch is reported" "${perturbed_err}" "verify: report [0-9]+ differs")


# --------------------------------------------------------------- storage modes

run(shared --shared-arena --quiet)
require_exit("shared arena exits 0" "${shared_rc}" "0")
require_match("shared arena is one arena" "${shared_out}"
   "Arenas            : 1 shared by every window, [0-9]+ pages held")

run(shared_verify --shared-arena --verify --quiet)
require_exit("shared arena rejects --verify" "${shared_verify_rc}" "1")
require_match("shared arena says why" "${shared_verify_err}" "drop --baseline/--verify")

run(perwindow --quiet)
require_match("per-window arenas are released" "${perwindow_out}"
   "Arenas            : 1250 created and released, peak [0-9]+ page\\(s\\) each")


# ------------------------------------------------------------------ live mode

run(live --live --stream-len 400 --quiet)
require_exit("live mode exits 0" "${live_rc}" "0")
require_match("live mode draws a bar" "${live_out}" "active \\[#+\\.*\\] [0-9]+/8")


# ------------------------------------------------------------------ bad input

run(zero_window --window-size 0)
require_exit("--window-size 0 fails" "${zero_window_rc}" "1")
require_match("--window-size 0 explains" "${zero_window_err}" "must be at least 1")

run(huge_active --max-active 999)
require_exit("--max-active 999 fails" "${huge_active_rc}" "1")
require_match("--max-active 999 explains" "${huge_active_err}" "limited to 256")

run(unknown --not-an-option)
require_exit("an unknown option fails" "${unknown_rc}" "1")
require_match("an unknown option explains" "${unknown_err}" "unknown option")

run(help --help)
require_exit("--help exits 0" "${help_rc}" "0")
# argv[0] again: a path, and with an extension on Windows
require_match("--help prints the usage" "${help_out}" "usage: .*windowrb[^ ]* \\[options\\] \\[FILE\\]")
require_match("--help lists the modes" "${help_out}" "--baseline")
require_match("--help lists the test hook" "${help_out}" "--perturb")


get_property(failed GLOBAL PROPERTY failures)
if(failed)
   list(LENGTH failed n)
   message(FATAL_ERROR "${n} windowrb test(s) failed: ${failed}")
endif()

message(STATUS "all windowrb tests passed")
