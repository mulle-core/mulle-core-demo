#
# Running a cross build's programs from a test.
#
# ctest runs on the host, but the programs under test are built for another
# system and cannot simply be executed here. CMake has an answer for this --
# CMAKE_CROSSCOMPILING_EMULATOR -- but it reaches `add_test(COMMAND <target>)`,
# and these tests are scripts: each is handed a program *path* and runs it
# itself, from a `cmake -P` process. So the emulator has to travel into the
# script, and this file holds both ends of that journey.
#
#   mulle_demo_emulator()   configure time, from the top level CMakeLists.txt
#   demo_program( VAR)      script time, from each test/run.cmake
#
# Nothing here does anything when the programs run natively, which is every
# build that is not a cross build: the emulator is empty, the command is the
# program and the tests are the ones they always were.
#


#
# Sets MULLE_DEMO_EMULATOR to what should run the programs under test. It stays
# unset when they run natively, which is every build that is not a cross build.
#
# An explicit -DMULLE_DEMO_EMULATOR wins, then a toolchain's choice of
# CMAKE_CROSSCOMPILING_EMULATOR, and failing both, wine for a Windows cross
# build -- the one case we know about and can therefore look for. Any other
# system's emulator is named by whoever set the cross build up; with none named
# and none found, the warning below says so at configure time, because the tests
# that follow would otherwise fail looking like the program's fault.
#
function( mulle_demo_emulator)
   # An explicit -D wins, including an explicit empty one: that says these
   # binaries run natively here, so do not wrap them. (A cache entry exists
   # exactly when somebody asked for one, which is why an absent entry and an
   # empty one are not the same state.)
   if(DEFINED CACHE{MULLE_DEMO_EMULATOR})
      return()
   endif()

   if(CMAKE_CROSSCOMPILING_EMULATOR)
      set(MULLE_DEMO_EMULATOR "${CMAKE_CROSSCOMPILING_EMULATOR}" CACHE STRING
          "Command that runs the programs under test, for a cross build")
      return()
   endif()

   if(NOT CMAKE_CROSSCOMPILING)
      return()
   endif()

   find_program( MULLE_DEMO_WINE wine)

   if(NOT MULLE_DEMO_WINE)
      message(WARNING
         "Cross-compiling for ${CMAKE_SYSTEM_NAME} with no emulator to test "
         "with. The tests run the programs they build, so they cannot pass "
         "here: install wine, or configure with "
         "-DMULLE_DEMO_EMULATOR=<command>.")
      return()
   endif()

   set(MULLE_DEMO_EMULATOR "${MULLE_DEMO_WINE}" CACHE STRING
       "Command that runs the programs under test, for a cross build")
endfunction()


#
# Turns one of a script's program variables into the command that runs it: the
# emulator, if there is one, followed by the program.
#
# Expand it *unquoted* at the exec site --
#
#   execute_process(COMMAND ${HASHSPACE} ${ARGN} ...)
#
# -- because that is what makes a two word command two arguments. An unquoted
# expansion is split on ';' and never on spaces, so a program whose path has a
# space in it still arrives as one argument, and quoting it would collapse the
# emulator and the program back into one broken word instead.
#
# EMULATOR is undefined when a script is run by hand, as its own usage comment
# describes, and then this does nothing -- which is also what happens on every
# native build.
#
function( demo_program VAR)
   if(NOT EMULATOR)
      return()
   endif()

   set(${VAR} ${EMULATOR} "${${VAR}}" PARENT_SCOPE)
endfunction()
