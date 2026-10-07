#
# The two things every demo's CMakeLists.txt would otherwise spell out again:
# the warning flags its targets are built with, and the add_test() block that
# hands its test script the programs the script runs.
#
# Both are boilerplate with a detail inside them that is easy to drop. A target
# that misses -Wall goes on compiling, just not like its neighbours, and nothing
# says so. A test that misses the emulator fails only in a cross build, and fails
# looking like the program's fault. So they are written once here, and each
# demo's CMakeLists.txt gets to be about what it builds.
#
#   mulle_demo_warnings( <target>...)   configure time, from a demo
#   mulle_demo_test( <name> ...)        configure time, from a demo
#
# Included from the top level CMakeLists.txt before the demos are added, so both
# are defined for all of them. A test is handed the emulator decided by
# cmake/cross-emulator.cmake, so mulle_demo_emulator() has to have run first;
# the top level does that.
#


#
# Give targets the warning flags every demo is built with.
#
# MSVC is skipped. -Wall is /Wall to it, which is not "the usual warnings" but
# every warning it has -- the ones about its own headers included -- and it
# already warns at /W3 without being asked. The flags below say what we mean on
# the compilers that spell it this way, which is every build here.
#
function( mulle_demo_warnings)
   if(MSVC)
      return()
   endif()

   foreach(target IN LISTS ARGN)
      target_compile_options(${target} PRIVATE -Wall -Wextra)
   endforeach()
endfunction()


#
# Register a demo's test: NAME is the ctest name, and the script is that demo's
# test/run.cmake, run by `cmake -P` with the variables every test script asks for
# at the top. Three keywords are named here and everything else is implied; each
# one is repeated when there is more than one of it.
#
#   PROGRAM <variable> <target>      becomes  -D<variable>=$<TARGET_FILE:target>
#   ARG <variable> <value>           becomes  -D<variable>=<value>
#   FIXTURES                         becomes  -DFIXTURES=<demo>/fixtures
#
# PROGRAM is the path of a built target and not a command: the script receives
# the binary and decides for itself whether to run it through the emulator
# (demo_program()). That is what makes a cross build's tests work, and it is also
# why every program the test starts itself belongs here -- miniredis' SERVER and
# the stand-in server it compares against are both a PROGRAM, and both arrive in
# the script as a plain path of the target system.
#
# ARG is for what a script cannot work out on its own, because `cmake -P` has no
# project to ask: the target system's name, or a capability the configure step
# decided and the script has to agree with (see redis/CMakeLists.txt).
#
# WORKDIR and EMULATOR are added unconditionally, and no test may omit them.
#
#
# The arguments are read here rather than with cmake_parse_arguments(), because
# that call cannot tell a misspelled keyword from a value: FIXTURE for FIXTURES
# would be eaten as the last element of the PROGRAM list -- its values have no
# fixed count, so it does not stop until the next keyword it knows -- and the
# mistake would come out later as a generator expression error about an empty
# target. This loop stops at every word and says which one it did not know.
#
# list(POP_FRONT) pads a short list with empty values instead of failing, so the
# length is checked before the two operands are popped.
#
function( mulle_demo_test NAME)
   set(_arguments ${ARGN})
   set(_definitions "")

   while(_arguments)
      list(POP_FRONT _arguments _keyword)

      if("${_keyword}" STREQUAL "FIXTURES")
         list(APPEND _definitions
              "-DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
      elseif("${_keyword}" STREQUAL "PROGRAM" OR "${_keyword}" STREQUAL "ARG")
         list(LENGTH _arguments _left)

         if(_left LESS 2)
            message(FATAL_ERROR
               "mulle_demo_test(${NAME}): ${_keyword} wants a variable and a "
               "value, and '${_arguments}' is not both")
         endif()

         list(POP_FRONT _arguments _variable _value)

         if("${_keyword}" STREQUAL "PROGRAM")
            list(APPEND _definitions "-D${_variable}=$<TARGET_FILE:${_value}>")
         else()
            list(APPEND _definitions "-D${_variable}=${_value}")
         endif()
      else()
         message(FATAL_ERROR
            "mulle_demo_test(${NAME}): '${_keyword}' is not FIXTURES, PROGRAM "
            "or ARG")
      endif()
   endwhile()

   add_test(NAME ${NAME}
            COMMAND ${CMAKE_COMMAND}
                    ${_definitions}
                    -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
                    "-DEMULATOR=${MULLE_DEMO_EMULATOR}"   # runs the programs
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/run.cmake)
endfunction()
