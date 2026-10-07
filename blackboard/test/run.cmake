#
# Fixture tests for blackboard, run by ctest or directly:
#
#   cmake -DBLACKBOARD=<binary> -DWORKDIR=<dir> -P test/run.cmake
#
# What is asserted is structure and accounting, not timing. The interesting
# assertions are the ones that pin the claims the demo makes:
#
#   * a run with the board lock is exact: every note accounted for, the shared
#     heap empty at the end, no payload corrupt
#   * a run with the tally unlocked drifts, and --verify has to notice
#   * a run that cannot fit its notes stays alive, counts the refusals, and
#     still accounts for every note
#   * a run that grows instead refuses only at its limit, and it grows without
#     gaining a segment, because the window is one mapping
#
# What is *not* asserted is anything the allocator is free to choose: the arena
# format version, the initial capacity, the size of a block. Those change when
# the implementation does its job, so a literal here would only ever fail for a
# reason no user of the allocator can act on -- and a suite that goes red for
# that teaches you to ignore red.
#
# The wall clock and the note rate are the machine's and are never matched.
#

foreach(required BLACKBOARD WORKDIR)
   if(NOT DEFINED ${required})
      message(FATAL_ERROR "${required} is not defined")
   endif()
endforeach()

# A cross build's binary cannot be run by this host, so it is run through an
# emulator when there is one. Unquoted from here on: BLACKBOARD may be two words.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/cross-emulator.cmake")
demo_program( BLACKBOARD)

string(ASCII 27 ESC)


function(run NAME)
   execute_process(COMMAND ${BLACKBOARD} ${ARGN}
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


function(require_exit NAME ACTUAL EXPECTED)
   if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
      message("FAIL ${NAME}: exit ${ACTUAL}, expected ${EXPECTED}")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


# finds the number after a label and puts it in ${NAME}
function(number_after NAME HAYSTACK PATTERN)
   if(NOT "${HAYSTACK}" MATCHES "${PATTERN}")
      message("FAIL ${NAME}: no number for [${PATTERN}] in [${HAYSTACK}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
      set(${NAME} "?" PARENT_SCOPE)
      return()
   endif()
   set(${NAME} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()


function(require_sum NAME A B EXPECTED)
   if("${A}" STREQUAL "?" OR "${B}" STREQUAL "?")
      return()
   endif()
   math(EXPR total "${A} + ${B}")
   if(NOT total EQUAL ${EXPECTED})
      message("FAIL ${NAME}: ${A} + ${B} = ${total}, expected ${EXPECTED}")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


# ------------------------------------------------------------------ the board

# 4 workers x 20000 notes, 1 monitor + 2 readers. Every note has to be either
# published or refused, every delivery either arrived or been dropped as the
# mailbox filled, and the board has to be empty at the end.
run(board --verify --quiet)

require_exit("a locked run exits 0" "${board_rc}" "0")
require_match("every note is published" "${board_out}"
   "published         80000 notes, 0 skipped \\(no subscriber\\)")
require_match("nothing is corrupt" "${board_out}"
   "contents          3 subscribers verified [0-9]+ payloads, 0 corrupt")
require_match("allocation and freeing balance" "${board_out}"
   "allocator         0 failures, 80000 allocations, 80000 frees")
require_match("the board ends empty" "${board_out}"
   "board heap        0.0 B live, [0-9.]+ KiB peak, 256.0 KiB of the region")
require_match("the invariants hold" "${board_out}"
   "verification      ok: every note accounted for, deliveries conserved, board empty, contents intact")

# The board is one mapping for the whole family of processes, and its own
# overhead has to stay small enough that the notes are what fills it.
require_match("one mapping, every process" "${board_out}"
   "region            256.0 KiB shared, one mapping, 7 processes")
require_match("the board's own chunk is small" "${board_out}"
   "board overhead    6.4 KiB \\(2.5 % of the region\\)")

# Each subscriber accounts for what it was offered: arrived + dropped.
require_match("each subscriber reports" "${board_out}"
   "   0 \\(monitor\\) +[0-9]+ +[0-9]+ +0 +4/4")

number_after(published "${board_out}" "published +([0-9]+) notes")
number_after(refused "${board_out}" "allocator +([0-9]+) failures")
require_sum("published + refused is every note that was offered" "${published}" "${refused}" 80000)


# ------------------------------------------------------------- the unchecked tally

# --unsafe reads the tally, publishes, and writes it back without the lock. The
# notes still arrive -- that is the point: what breaks is the number a monitor
# would be reading, and this run is what proves --verify is looking at it.
#
# One worker has to still be publishing while another one is, for there to be
# anything to lose, and that is a question of how long a worker takes against how
# long the monitor needs to start the family. At the default 20000 notes a worker
# is done in ~10 ms, which fits inside starting six processes on a slow host
# (wine needs ~200 ms), and the lost updates then simply never happen. 100000
# notes is long enough on every host measured -- 0.3 s per worker on wine -- and
# the assertion stays as weak as it was: that the drift is noticed, never how
# large it is.
run(unsafe --unsafe --verify --quiet --notes 100000)

require_exit("an unlocked tally fails verification" "${unsafe_rc}" "1")
require_match("the drift is named" "${unsafe_out}" "lost updates")
require_match("the notes themselves are still fine" "${unsafe_out}"
   "contents          3 subscribers verified [0-9]+ payloads, 0 corrupt")
require_match("and it is reported as a failure" "${unsafe_out}"
   "verification      FAILED \\(1\\)")


# ------------------------------------------------------------------ exhaustion

# A 16 KiB board cannot hold the notes. Publishing has to keep going, count the
# refusals and leave the board intact -- and the accounting still has to add up,
# because a refused note is a counted outcome, not a lost one.
run(exhaust --exhaust --verify --quiet)

require_exit("exhaustion does not kill it" "${exhaust_rc}" "0")
require_match("refusals are counted" "${exhaust_out}"
   "allocator         [1-9][0-9]+ failures, [0-9]+ allocations, [0-9]+ frees")
require_match("it says which room it had" "${exhaust_out}"
   "verification      ok: .* \\(within the room the board had\\)")

number_after(exhaust_published "${exhaust_out}" "published +([0-9]+) notes")
number_after(exhaust_refused "${exhaust_out}" "allocator +([0-9]+) failures")
require_sum("a refused note is still accounted for" "${exhaust_published}" "${exhaust_refused}" 80000)


# -------------------------------------------------------------------- growth

# The same board with --grow: instead of refusing the moment its first region is
# full, it expands into the window it reserved up front. The window is a single
# mapping, so growing it *extends* the segment the arena started with -- the
# capacity moves and the segment count stays at one. Past --growth-limit a note
# is refused exactly as before, so the accounting has to add up either way.
#
# The board's own allocation fits in the first segment, so it is the workers --
# child processes -- that have to grow it, and the monitor must not. The readers
# are the sharpest witness of all: they never allocate, so they cannot grow
# anything, yet they take notes out of a segment a worker added.
run(grow --exhaust --grow --growth-limit 64K --verify --dump --quiet)

require_exit("a growing board exits 0" "${grow_rc}" "0")
require_match("it says what it can grow to" "${grow_out}"
   "region            16.0 KiB shared growing to 64.0 KiB, one mapping, 7 processes")
#  That the count stayed at one is the claim, not a detail: a second segment
#  would mean the arena mapped something new, which this embedder cannot do.
require_match("growth extends the one segment" "${grow_out}"
   "growth            1 segment, 64.0 KiB of 64.0 KiB limit")
require_match("the invariants hold while growing" "${grow_out}"
   "verification      ok: every note accounted for, deliveries conserved, board empty, contents intact")

# Attribution: a child grew the board, and the monitor did not.
require_match("the monitor never grows the board" "${grow_out}"
   "growth source     monitor added 0 segments")
require_match("a worker grows the board" "${grow_out}"
   "[1-9][0-9]* of 4 workers added [1-9][0-9]*")
# And a reader, which never allocates and so cannot grow anything itself, still
# takes notes that live in the segment the worker added.
require_match("a process that cannot grow takes notes from the growth" "${grow_out}"
   "readers took [1-9][0-9]* notes from grown segments")

number_after(grow_published "${grow_out}" "published +([0-9]+) notes")
number_after(grow_refused "${grow_out}" "allocator +([0-9]+) failures")
require_sum("a grown board still accounts for every note" "${grow_published}" "${grow_refused}" 80000)

#  The dump is the allocator's own account of the growth: the window is used up
#  to the limit, still in one segment. `arena` is the initial capacity, which is
#  the allocator's to pick, so only its presence is asserted.
require_match("the arena grew into the whole window" "${grow_err}"
   "capacity=65536  arena=[0-9]+  segments=1")
require_match("and the grown bytes are in that segment" "${grow_err}"
   "segment 0 at offset 0, 65536 bytes")


# -------------------------------------------------------------------- the heap

# The dump is mulle-mmapallocator's view of the very same region: the arena
# header, then the block tiling of every segment. The board itself is the one
# allocation still in use, and every note has been given back.
run(dump --dump --notes 500 --quiet)

require_exit("dump exits 0" "${dump_rc}" "0")
require_match("the arena is dumped" "${dump_err}"
   "=== mulle_mmap_allocator dump ===")
# The address is printed with %p, and how a C library spells one is its own
# business: glibc writes 0x..., the Windows CRT does not. What is asserted is
# that a mapping address is there at all, and that the arena it describes is the
# whole region in one segment.
require_match("it is the same region" "${dump_err}"
   "capacity=262144  arena=262144  segments=1")
#  The format version is the implementation's to bump, and bumping it is what a
#  format change looks like, so the literal is not a contract. What is asserted
#  is that the dump names one at all.
require_match("the arena header is dumped with a format version" "${dump_err}"
   "version=[0-9]+")
require_match("segment 0 is the whole region" "${dump_err}"
   "segment 0 at offset 0, 262144 bytes")
# every note has been consumed and freed, so the region is one free block again
# next to the board: the allocator gives the space back to itself, not to the OS
require_match("the board is in use" "${dump_err}" "INUSE  size=[0-9]+")
require_match("the notes come back as free space" "${dump_err}"
   "FREE   size=[0-9][0-9][0-9][0-9][0-9][0-9]*")


# ------------------------------------------------------------------- interfaces

# --show decodes the notes the monitor receives, and they verify against what
# (seq, seed) say they must contain.
run(show --show 3 --notes 400 --quiet)
require_exit("show exits 0" "${show_rc}" "0")
require_match("a note is decoded" "${show_out}"
   "\\[[0-9]+\\] worker [0-9]+ -> monitor, topic [0-9]+, [0-9]+ B, verified")

# Anything that lands in a log file has to be readable text, so with the tui off
# there must be no cursor control at all.
run(notui --no-tui --notes 4000 --quiet)
require_match("quiet still summarises" "${notui_out}" "=== Blackboard ===")
string(FIND "${notui_out}" "${ESC}[" quiet_escape)
if(quiet_escape EQUAL -1)
   message(STATUS "ok   --quiet paints nothing")
else()
   message("FAIL --quiet paints nothing: found an escape at ${quiet_escape}")
   set_property(GLOBAL APPEND PROPERTY failures "--quiet paints nothing")
endif()

# The inline (non-tui) report, reached by asking for one sooner rather than by
# being slow. The default cadence is half a second, and a machine can finish the
# whole run inside that -- the Mac publishes 400000 notes in 235 ms -- so
# relying on the default would make this a stopwatch on the host rather than a
# test of the demo. --interval is what makes it deterministic on any machine.
run(plain --no-tui --notes 100000 --interval 0.05)
string(FIND "${plain_out}" "${ESC}[" plain_escape)
if(plain_escape EQUAL -1)
   message(STATUS "ok   --no-tui paints nothing")
else()
   message("FAIL --no-tui paints nothing: found an escape at ${plain_escape}")
   set_property(GLOBAL APPEND PROPERTY failures "--no-tui paints nothing")
endif()
require_match("--no-tui reports inline" "${plain_out}"
   "[0-9]+\\.[0-9][0-9] s  published [0-9]+  delivered [0-9]+")


# ------------------------------------------------------------------ corruption

# Teeth for the content check: --perturb flips a payload byte after its checksum
# was taken, so the notes still arrive and only their contents are wrong. If the
# reader stops comparing, this test goes quiet.
run(perturb --perturb --verify --quiet)

require_exit("a corrupted payload is caught" "${perturb_rc}" "1")
require_match("the reader notices" "${perturb_out}"
   "contents          3 subscribers verified [0-9]+ payloads, [1-9][0-9]* corrupt")
require_match("and verification reports it" "${perturb_out}"
   "[1-9][0-9]* of [0-9]+ delivered notes did not verify")


# -------------------------------------------------------------------- bad input

run(no_workers --workers 0)
require_exit("--workers 0 fails" "${no_workers_rc}" "1")
require_match("--workers 0 explains" "${no_workers_err}" "at least 1")

run(no_topics --topics 0)
require_exit("--topics 0 fails" "${no_topics_rc}" "1")
require_match("--topics 0 explains" "${no_topics_err}" "must be 1\\.\\.")

run(tiny_payload --payload 4)
require_exit("--payload 4 fails" "${tiny_payload_rc}" "1")
require_match("--payload 4 explains" "${tiny_payload_err}" "must be 17\\.\\.")

# A 4 KiB region cannot hold the board, so the refusal path is what has to
# answer. The allocator escalates a request to the platform's page size first,
# so this asks what a page is on each of them: 4 KiB here, and 4 KiB on Windows
# too, where dwPageSize is the answer (dwAllocationGranularity, the 64 KiB
# number, is for offsets and base addresses instead).
run(tiny_board --capacity 4K)
require_exit("a board too small to hold itself fails" "${tiny_board_rc}" "1")
require_match("and says why" "${tiny_board_err}" "cannot fit a 3 subscriber board")

run(unknown --not-an-option)
require_exit("an unknown option fails" "${unknown_rc}" "1")
require_match("an unknown option prints the usage" "${unknown_err}" "usage: blackboard")

# The usage goes to stderr, so that `blackboard --help > file` is not a way to
# capture a summary by accident.
run(help --help)
require_exit("--help exits 0" "${help_rc}" "0")
require_match("--help prints the usage" "${help_err}" "usage: blackboard \\[options\\]")
require_match("--help lists the failure mode" "${help_err}" "--unsafe")
require_match("--help lists exhaustion" "${help_err}" "--exhaust")
require_match("--help lists the test hook" "${help_err}" "--perturb")


get_property(failed GLOBAL PROPERTY failures)
if(failed)
   list(LENGTH failed n)
   message(FATAL_ERROR "${n} blackboard test(s) failed: ${failed}")
endif()

message(STATUS "all blackboard tests passed")
