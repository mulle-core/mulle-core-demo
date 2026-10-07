#
# Fixture tests for hashspace, run by ctest or directly:
#
#   cmake -DHASHSPACE=<binary> -DWORKDIR=<dir> -DASSERTIONS=ON -P test/run.cmake
#
# What is asserted is the contract, not the speed. The rows of --space are the
# documented hash spaces, --replace is the no-replace protocol, --churn checks
# that nothing is lost or corrupt, and --mask/--universe pin the two modes where
# the call does not come back at all.
#
# The wall clock, the op rate and the latency percentiles are the machine's and
# are never matched.

foreach(required HASHSPACE WORKDIR ASSERTIONS)
   if(NOT DEFINED ${required})
      message(FATAL_ERROR "${required} is not defined")
   endif()
endforeach()

# A cross build's binary cannot be run by this host, so it is run through an
# emulator when there is one. Unquoted from here on: HASHSPACE may be two words.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/cross-emulator.cmake")
demo_program( HASHSPACE)


function(run NAME)
   execute_process(COMMAND ${HASHSPACE} ${ARGN}
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


function(require_no_match NAME HAYSTACK PATTERN)
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


# finds the number after a label and puts it in ${NAME}
function(number_after NAME HAYSTACK PATTERN)
   if(NOT "${HAYSTACK}" MATCHES "${PATTERN}")
      message("FAIL ${NAME}: no number for [${PATTERN}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
      set(${NAME} "?" PARENT_SCOPE)
      return()
   endif()
   set(${NAME} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()


# asserts that the numbers passed add up to the last one
function(require_sum NAME EXPECTED)
   set(total 0)
   foreach(term ${ARGN})
      if("${term}" STREQUAL "?")
         return()
      endif()
      math(EXPR total "${total} + ${term}")
   endforeach()
   if(NOT total EQUAL ${EXPECTED})
      message("FAIL ${NAME}: ${ARGN} = ${total}, expected ${EXPECTED}")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


function(require_equal NAME A B)
   if("${A}" STREQUAL "?" OR "${B}" STREQUAL "?")
      return()
   endif()
   if(NOT "${A}" STREQUAL "${B}")
      message("FAIL ${NAME}: ${A} != ${B}")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


function(require_gt NAME ACTUAL THRESHOLD)
   if("${ACTUAL}" STREQUAL "?")
      return()
   endif()
   if(NOT "${ACTUAL}" GREATER "${THRESHOLD}")
      message("FAIL ${NAME}: ${ACTUAL} is not greater than ${THRESHOLD}")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


# ------------------------------------------------------------------ hash space

# One row per candidate hash: what positive mode, even mode and the hashmap do
# with it. These are the rules the demo exists to state, so they are pinned
# exactly -- including that a rejection is EINVAL rather than a folded hash.
run(space --space --quiet)

require_exit("--space exits 0" "${space_rc}" "0")
require_match("the hash width is reported" "${space_out}" "space\\.intptr_bits=[0-9]+")
require_match("0 is refused everywhere" "${space_out}"
   "space\\.0=positive:EINVAL,even:EINVAL,hashmap:EINVAL")
require_match("an odd hash is legal in positive mode only" "${space_out}"
   "space\\.1=positive:ok,even:EINVAL,hashmap:ok")
require_match("the top bit is positive mode's frozen flag" "${space_out}"
   "space\\.INTPTR_MAX=positive:ok,even:EINVAL,hashmap:ok")
require_match("-1 is out of both hashtable spaces" "${space_out}"
   "space\\.-1=positive:EINVAL,even:EINVAL,hashmap:ok")
require_match("-2 is out of positive only" "${space_out}"
   "space\\.-2=positive:EINVAL,even:ok,hashmap:ok")
require_match("INTPTR_MIN is out of positive only" "${space_out}"
   "space\\.INTPTR_MIN=positive:EINVAL,even:ok,hashmap:ok")
require_match("an aligned pointer is even" "${space_out}"
   "space\\.malloc\\(\\)=positive:ok,even:ok,hashmap:ok")
require_match("an unaligned pointer is refused by even mode" "${space_out}"
   "space\\.malloc\\(\\)\\+1=positive:ok,even:EINVAL,hashmap:ok")

run(space_human --space)

require_exit("the reported table exits 0" "${space_human_rc}" "0")
require_match("it has a column per map" "${space_human_out}" "positive +even +hashmap")
require_match("and says rejection is not truncation" "${space_human_out}"
   "never a silent truncation")


# --------------------------------------------------------------------- replace

run(replace --replace --quiet)

require_exit("--replace exits 0" "${replace_rc}" "0")
require_match("the first insert lands" "${replace_out}" "replace\\.insert_a=ok")
require_match("a second insert on the same hash is refused" "${replace_out}"
   "replace\\.insert_b=EEXIST")
require_match("and the first value survives it" "${replace_out}"
   "replace\\.lookup=A \\(B was refused\\)")
require_match("removing with the wrong value is refused" "${replace_out}"
   "replace\\.remove_b=ENOENT")
require_match("removing with the right value works" "${replace_out}"
   "replace\\.remove_a=ok")
require_match("and the hash is insertable again at once" "${replace_out}"
   "replace\\.insert_reuse=ok")
require_match("register reports the value it did not displace" "${replace_out}"
   "replace\\.register_live=ok p_old=B")
require_match("and NULL when it did insert" "${replace_out}"
   "replace\\.register_free=ok p_old=NULL")
require_match("the hashmap register reports the insertion" "${replace_out}"
   "replace\\.hashmap_register_empty=NO_POINTER \\(inserted\\)")
require_match("the hashmap insert refuses a duplicate" "${replace_out}"
   "replace\\.hashmap_insert_again=EEXIST")
require_match("the hashmap patch is a plain replace" "${replace_out}"
   "replace\\.hashmap_patch=ok, lookup now 0xa")
require_match("and its remove is that call with NULL" "${replace_out}"
   "replace\\.hashmap_remove=ok, lookup now NULL")


# ----------------------------------------------------------------------- churn

# Two threads, 20000 operations each, no migration: nothing may be lost, no slot
# may hold a value that does not belong to its hash, and the table has to be
# drainable afterwards.
run(churn --churn --quiet --threads 2 --ops 20000 --enumerate --verify)

require_exit("a clean churn exits 0" "${churn_rc}" "0")
require_match("no hash is ever out of space" "${churn_out}" "churn\\.einval=0")
require_match("no slot ever holds the wrong value" "${churn_out}"
   "churn\\.corrupt=0 churn\\.failed=0")
require_match("nothing migrates on its own" "${churn_out}" "churn\\.migrations=0")
require_match("so no walk is cancelled" "${churn_out}" "churn\\.restarts=0")

number_after(ops           "${churn_out}" "churn\\.ops=([0-9]+)")
number_after(ins_attempts  "${churn_out}" "churn\\.insert_attempts=([0-9]+)")
number_after(ins_ok        "${churn_out}" "churn\\.inserted=([0-9]+)")
number_after(ins_eexist    "${churn_out}" "churn\\.eexist=([0-9]+)")
number_after(ins_einval    "${churn_out}" "churn\\.einval=([0-9]+)")
number_after(rem_attempts  "${churn_out}" "churn\\.remove_attempts=([0-9]+)")
number_after(rem_ok        "${churn_out}" "churn\\.removed=([0-9]+)")
number_after(rem_enoent    "${churn_out}" "churn\\.enoent=([0-9]+)")
number_after(lookups       "${churn_out}" "churn\\.lookups=([0-9]+)")
number_after(hits          "${churn_out}" "churn\\.hits=([0-9]+)")
number_after(misses        "${churn_out}" "churn\\.misses=([0-9]+)")
number_after(live          "${churn_out}" "churn\\.live=([0-9]+)")
number_after(drained       "${churn_out}" "churn\\.drained=([0-9]+)")

require_sum("every insert landed or was refused"
   "${ins_attempts}" "${ins_ok}" "${ins_eexist}" "${ins_einval}")
require_sum("every remove removed or was refused"
   "${rem_attempts}" "${rem_ok}" "${rem_enoent}")
require_sum("every lookup hit or missed" "${lookups}" "${hits}" "${misses}")
require_sum("every operation was one of the three"
   "${ops}" "${ins_attempts}" "${rem_attempts}" "${lookups}")
require_equal("the verification walk drained everything" "${live}" "${drained}")


# ------------------------------------------------------- the two maps compared

# The hashmap has no concurrent remove, so it cannot run the three way mix, and
# --no-remove asks the hashtable for the same two operations instead. The op is
# drawn from the seed before the map is touched, so the two runs start from the
# very same operations: the counters that do not depend on thread timing have to
# be equal, not merely close. Hit and miss counts are deliberately not compared,
# they depend on whether another thread had inserted the key yet.
run(churn_noremove --churn --no-remove --quiet --threads 2 --ops 20000 --enumerate --verify)
run(churn_hashmap  --churn --map hashmap --quiet --threads 2 --ops 20000 --enumerate --verify)

require_exit("the hashtable on the hashmap's mix exits 0" "${churn_noremove_rc}" "0")
require_exit("the hashmap churn exits 0" "${churn_hashmap_rc}" "0")
require_match("the run names the map and the mix" "${churn_hashmap_out}"
   "churn\\.map=hashmap churn\\.mix=insert,lookup churn\\.slots=4096")
require_match("the hashtable says it is on the same mix" "${churn_noremove_out}"
   "churn\\.map=hashtable churn\\.mix=insert,lookup")
require_match("the mix has no removes at all" "${churn_hashmap_out}"
   "churn\\.remove_attempts=0 churn\\.removed=0 churn\\.enoent=0")
require_match("nothing is corrupt in the hashmap either" "${churn_hashmap_out}"
   "churn\\.corrupt=0 churn\\.failed=0")
require_match("no hash is out of space in the hashmap either" "${churn_hashmap_out}"
   "churn\\.einval=0")
require_match("and nothing ever migrates for it to cancel a walk over" "${churn_hashmap_out}"
   "churn\\.migrations=0 churn\\.walks=[0-9]+ churn\\.restarts=0")

foreach(counter ops insert_attempts inserted eexist einval lookups remove_attempts live)
   number_after(htn_${counter} "${churn_noremove_out}" "churn\\.${counter}=([0-9]+)")
   number_after(hm_${counter}  "${churn_hashmap_out}"  "churn\\.${counter}=([0-9]+)")
   require_equal("${counter} is the same in both runs" "${htn_${counter}}" "${hm_${counter}}")
endforeach()

number_after(hm_drained "${churn_hashmap_out}" "churn\\.drained=([0-9]+)")
number_after(hm_live    "${churn_hashmap_out}" "churn\\.live=([0-9]+)")

require_gt("the hashmap's inserts landed" "${hm_inserted}" "0")
require_equal("and its single threaded drain emptied it" "${hm_live}" "${hm_drained}")

run(churn_hashmap_human --churn --map hashmap --threads 1 --ops 1000)

require_exit("the human hashmap churn exits 0" "${churn_hashmap_human_rc}" "0")
require_match("it explains why there are no removes" "${churn_hashmap_human_out}"
   "single threaded writes")
require_match("and names the comparable hashtable run" "${churn_hashmap_human_out}"
   "--churn --no-remove --threads 1 --ops 1000 --keys")
require_match("the table did not grow" "${churn_hashmap_human_out}" "4096 slots")


# ------------------------------------------------------------------- migration

# The same churn with forced generations and an enumerating thread. Storage is
# retired the whole time, so the walks cross generation changes and frozen slots
# and get told to restart -- and after all of that the table still has to hold
# exactly the values its hashes imply.
run(churn_migrate --churn --quiet --threads 4 --ops 100000 --migrate --verify --enumerate)

require_exit("a churning migration exits 0" "${churn_migrate_rc}" "0")
require_match("still nothing is corrupt" "${churn_migrate_out}"
   "churn\\.corrupt=0 churn\\.failed=0")

number_after(migr_migrations "${churn_migrate_out}" "churn\\.migrations=([0-9]+)")
number_after(migr_walks      "${churn_migrate_out}" "churn\\.walks=([0-9]+)")
number_after(migr_restarts   "${churn_migrate_out}" "churn\\.restarts=([0-9]+)")
number_after(migr_live       "${churn_migrate_out}" "churn\\.live=([0-9]+)")
number_after(migr_drained    "${churn_migrate_out}" "churn\\.drained=([0-9]+)")

require_gt("generations were really forced" "${migr_migrations}" "0")
require_gt("the table was walked" "${migr_walks}" "0")
require_gt("and walks were cancelled by the migration" "${migr_restarts}" "0")
require_equal("and the table still drained completely" "${migr_live}" "${migr_drained}")


# ------------------------------------------------------------------------ mask

# A power of two size is the precondition for linear probing to terminate, and
# the caller no longer has to honour it: the storage allocator rounds the size up
# to the next power of two before it computes the mask. So a size that is not a
# power of two is a hint, not a hazard -- the hazard is what --mask still computes,
# against the size that was *asked for*, and it is printed next to the size the
# table actually used. That the two differ is the whole of the fix.
#
# Every row is asserted to come back, and that is one claim in both build
# configurations: the roundup is not an assert, so NDEBUG does not change it.
run(mask --mask --quiet)

require_exit("--mask exits 0" "${mask_rc}" "0")
require_match("a power of two table is the whole table" "${mask_out}"
   "mask\\.pow2=2048 mask\\.pow2_reachable=2048 mask\\.hashtable_pow2=completed, table 2048")
require_match("a size that is not a power of two would reach 32 of 2064 slots" "${mask_out}"
   "mask\\.nonpow2=2064 mask\\.nonpow2_reachable=32")
# 2064 asked for, 4096 used: the mask is 4095 and it covers every slot, which is
# why the lookup that used to walk a fraction of the table now comes back.
require_match("2064 is rounded up to 4096, so the mask covers the table" "${mask_out}"
   "mask\\.hashtable_nonpow2_insert=completed, table 2064 -> 4096")
require_match("the lookup in that table comes back too" "${mask_out}"
   "mask\\.hashtable_nonpow2_lookup=completed, table 2064 -> 4096")
require_match("and so does the hashmap's insert, which has no other escape" "${mask_out}"
   "mask\\.hashmap_nonpow2_insert=completed, table 2064 -> 4096")
require_match("while a power of two hashmap is fine" "${mask_out}"
   "mask\\.hashmap_pow2=completed, table 2048")


# -------------------------------------------------------------------- universe

# Retiring storage needs the ABA reclaimer. Forgetting it is latent: a map that
# never migrates never retires anything, so the small case completes and the one
# that grows does not.
run(universe --universe --quiet)

require_exit("--universe exits 0" "${universe_rc}" "0")
require_match("a small map survives without the reclaimer" "${universe_out}"
   "universe\\.small=completed, table 4")
require_no_match("and it never grew, which is why it never retired anything" "${universe_out}"
   "universe\\.small=completed, table 4 ->")
require_no_match("one that grows does not" "${universe_out}"
   "universe\\.grown=completed")
require_match("with mulle_aba_init it completes -- and the report shows the growth" "${universe_out}"
   "universe\\.with_init=completed, table 4 -> ")

run(universe_human --universe)

require_match("the allocator itself names the remedy" "${universe_human_err}"
   "no ABA reclaimer installed")
require_match("and the demo explains the per thread part" "${universe_human_out}"
   "mulle_aba_register\\(\\) once per thread")


# ------------------------------------------------------------------- bad input

run(unknown --not-an-option)

require_exit("an unknown option fails" "${unknown_rc}" "1")
require_match("and prints the usage" "${unknown_err}" "usage: hashspace")

run(map_unknown --map nonsense)

require_exit("an unknown map fails" "${map_unknown_rc}" "1")
require_match("and names it" "${map_unknown_err}" "unknown map nonsense")

# There is no hashmap migrate_same_size, so the combination is refused rather
# than quietly run without the migration.
run(migrate_hashmap --churn --map hashmap --migrate --quiet --ops 100)

require_exit("the hashmap has no forced migration, so --migrate is refused"
   "${migrate_hashmap_rc}" "1")
require_match("and says which one it needs" "${migrate_hashmap_err}"
   "no hashmap migrate_same_size")

run(migrate_hashtable --churn --map hashtable --migrate --quiet --ops 100 --threads 1)

require_exit("while --map hashtable --migrate runs" "${migrate_hashtable_rc}" "0")

run(help --help)

require_exit("--help exits 0" "${help_rc}" "0")
require_match("--help prints the usage" "${help_out}" "usage: hashspace \\[mode\\]")
require_match("--help lists the mask mode" "${help_out}" "--mask")
require_match("--help lists the map option" "${help_out}"
   "--map NAME +churn the hashtable \\(default\\) or the hashmap")
require_match("--help lists the universe mode" "${help_out}" "--universe")

# A thread count below 1 is clamped rather than refused, and the run says so.
run(clamped --churn --quiet --threads 0 --ops 100)

require_exit("a clamped thread count is not an error" "${clamped_rc}" "0")
require_match("and the run reports the clamp" "${clamped_out}" "churn\\.threads=1")


get_property(failed GLOBAL PROPERTY failures)
if(failed)
   list(LENGTH failed n)
   message(FATAL_ERROR "${n} hashspace test(s) failed: ${failed}")
endif()
