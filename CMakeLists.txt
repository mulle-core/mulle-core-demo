#
# Demos that consume mulle-core the way a downstream project does:
# a source checkout plus add_subdirectory. No mulle-sde and no install step.
#
cmake_minimum_required(VERSION 3.19...99.99)

project(mulle-core-demo LANGUAGES C)

#
# The dependencies are source checkouts, consumed the way a downstream project
# consumes them. The first place each one is looked for is next to this project,
# which is where a checkout of these repos has them; a clone of this repo alone
# has nothing there, so what is missing is fetched from the same GitHub
# organization instead. That is the difference between "works in the working
# copy" and "works for whoever clones this": nothing needs to be arranged by
# hand, and -DMULLE_DEMO_FETCH=OFF goes back to checkouts only for an offline or
# mirrored build.
#
set(MULLE_DEMO_GIT_BASE "https://github.com/mulle-core" CACHE STRING
    "Git base URL for dependencies not checked out next to this project")

set(MULLE_DEMO_GIT_TAG "" CACHE STRING
    "Branch or tag the dependencies are fetched at, empty for their default branch")

set(MULLE_DEMO_FETCH ON CACHE BOOL
    "Fetch dependencies that are not checked out next to this project")

#
# How a fetch is declared, everywhere below: SOURCE_SUBDIR points at a directory
# with no CMakeLists.txt in it, which is how FetchContent is told to only *get*
# the sources and not add them as a subdirectory. The demos stay the only place
# that adds a target, so a fetched build and a checkout build are put together
# the same way and land in the same place in the build tree.
#
# FetchContent_Declare() does not work from inside a function -- the module
# re-enters itself looking for the caller and hits its recursion limit -- so the
# declares are spelled out where they are used, with the tag as a variable.
#
set(_mulle_demo_fetch_tag "")

if(MULLE_DEMO_GIT_TAG)
   set(_mulle_demo_fetch_tag GIT_TAG "${MULLE_DEMO_GIT_TAG}")
endif()

set(MULLE_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mulle-core"
    CACHE PATH "Path to a mulle-core source checkout")

set(_mulle_core_dir "${MULLE_CORE_SOURCE_DIR}")

if(NOT EXISTS "${_mulle_core_dir}/CMakeLists.txt")
   if(NOT MULLE_DEMO_FETCH)
      message(FATAL_ERROR
         "No mulle-core checkout at '${MULLE_CORE_SOURCE_DIR}', and "
         "MULLE_DEMO_FETCH is off.\n"
         "Clone it next to this project, configure with "
         "-DMULLE_CORE_SOURCE_DIR=/path/to/mulle-core, or turn fetching back "
         "on (-DMULLE_DEMO_FETCH=ON).")
   endif()

   include(FetchContent)

   FetchContent_Declare(mulle-core
      GIT_REPOSITORY "${MULLE_DEMO_GIT_BASE}/mulle-core.git"
      ${_mulle_demo_fetch_tag}
      GIT_SHALLOW TRUE
      SOURCE_SUBDIR _mulle_demo_populate_only)
   FetchContent_MakeAvailable(mulle-core)

   set(_mulle_core_dir "${mulle-core_SOURCE_DIR}")
   message(STATUS "Using fetched mulle-core: ${_mulle_core_dir}")
endif()

#
# mulle-core's own test suite does not run here.
#
# add_subdirectory() brings mulle-core's test/ with it: 563 ctests over the
# vendored constituent tests. Their goldens were captured in a Debug build, and
# this project configures a Release one (-O3 -DNDEBUG), where several of them
# cannot pass for reasons that have nothing to do with the demos:
#
#   * mulle-rbtree/90-count spells its only add as
#     `assert( mulle_rbtree_add( ...))`, so under NDEBUG it adds nothing and
#     prints "walk 0, storage 0" against a golden made with 100 adds. 80-is-empty
#     and 95-capacity are the same shape.
#   * mulle-container/set does `#undef NDEBUG` before <assert.h> to keep its own
#     check live, and then trips over a Debug/Release difference in the headers.
#
# Those sources pass 563/563 in mulle-core's own build (cmake-build-debug), which
# is the right place for them: mulle-core runs its suite, the demos run the
# demos, and a demo that goes red for a vendored golden is a demo nobody trusts.
# -DMULLE_DEMO_CORE_TESTS=ON puts them back.
#
option(MULLE_DEMO_CORE_TESTS
   "Run mulle-core's own vendored test suite from this build" OFF)

if(NOT MULLE_DEMO_CORE_TESTS)
   #
   # A cache entry, not a plain variable. mulle-core's own include(CTest) calls
   # option() on this name, and option() honours a cache entry where it does not
   # reliably honour a normal variable inherited from an enclosing directory: a
   # plain set() here leaves the suite registered.
   #
   set(BUILD_TESTING OFF CACHE BOOL "Build mulle-core's own test tree" FORCE)
endif()

# mulle-core builds itself and everything it amalgamates as one target.
add_subdirectory("${_mulle_core_dir}" mulle-core)

#
# Name the include root ourselves.
#
# dox/DESIGN.md says a consumer is supposed to need nothing but
# add_subdirectory() + `#include <mulle-core/mulle-core.h>`, but nothing hands
# the include directory over: cmake/share/AddSubdirectorySupport.cmake has its
# target_include_directories(...) export commented out and skips header staging
# because "src/mulle-core" exists, and cmake/InstallCMakeInclude.cmake (which
# would attach it) is not included by anything and guards on
# MULLE_USE_TRANSITIVE_SOURCE_INCLUDE, one underscore short of the
# _MULLE_USE_TRANSITIVE_SOURCE_INCLUDE that AddSubdirectorySupport sets.
#
# Attaching it to the imported target keeps the workaround in one place.
# SYSTEM because mulle-core's headers use "#pragma mark", which -Wall turns
# into one warning per pragma when they are reached through a normal -I.
#
target_include_directories(mulle-core SYSTEM INTERFACE
   $<BUILD_INTERFACE:${_mulle_core_dir}/src>)

#
# The same gap, one platform further. mulle-dlfcn includes <dlfcn.h>
# unconditionally ("can be dlfcn-win32 or system"), and on Windows that header
# comes from the dlfcn-win32 that mulle-core carries in dlfcn-win32/. mulle-core
# adds it with a plain include_directories(), which is scoped to mulle-core's own
# directory, so a consumer compiling its own file that includes
# <mulle-core/mulle-core.h> never sees it and dies on 'dlfcn.h' file not found.
#
# mulle-core does compile dlfcn-win32/dlfcn.c into itself, so only the header is
# missing here -- attach the directory the same way as the include root above.
#
if(WIN32)
   target_include_directories(mulle-core SYSTEM INTERFACE
      $<BUILD_INTERFACE:${_mulle_core_dir}/dlfcn-win32>)

   #
   # And the same gap a third time, now for a system library. mulle-socket
   # calls winsock, and on Windows that is ws2_32; mulle-core declares it in
   # cmake/reflect/_Libraries.cmake, but an add_subdirectory consumer receives
   # only those OS libraries that happen to be cmake *targets* --
   # AddSubdirectorySupport.cmake filters the interface list on
   # `if( TARGET ...)`, and a find_library() result is a path, not a target. So
   # the consumer that reaches the socket code links it. A mulle-craft build of
   # mulle-core does not need this line.
   #
   target_link_libraries(mulle-core INTERFACE ws2_32)
endif()

# miniredis serves each connection on its own thread.
find_package(Threads REQUIRED)

#
# The tests run the programs they build, so a cross build needs something that
# can run them. Deciding what that is belongs here, once, rather than in each
# test; the demos pass the answer on to their test scripts. The long version of
# this is cmake/cross-emulator.cmake.
#

include(cmake/cross-emulator.cmake)
mulle_demo_emulator()

#
# The warning flags and the add_test() block that every demo would otherwise
# spell out again. The long version of both is cmake/mulle-demo.cmake.
#
include(cmake/mulle-demo.cmake)

enable_testing()

add_subdirectory(multigrep)
add_subdirectory(logtop)
add_subdirectory(redis)
add_subdirectory(urltoolkit)
add_subdirectory(windowrb)
add_subdirectory(hashspace)
add_subdirectory(blackboard)


#
# Give every demo the UTF-8 process manifest on Windows.
#
# The C runtime builds `argv` by converting the wide command line to the process
# ANSI code page -- the system locale's, e.g. CP1252 -- so a program that reads
# argv as UTF-8 receives mojibake for any non-ASCII argument, and the original
# bytes are already gone by the time main() runs. Declaring the process code page
# as UTF-8 makes that same conversion produce UTF-8.
#
# This is a property of each *executable*, not of something they link, so it is
# a function applied to a target list rather than an INTERFACE library. The
# resource path is absolute because windres runs from the target's build
# directory and would not find a relative one.
#
# Windows 10 1903 and later. Older systems ignore the setting, which is why no
# demo may depend on it for anything beyond argument text.
#
# Long version: cmake/utf8-executable.manifest
#
if(WIN32)
   enable_language(RC)

   set(MULLE_DEMO_UTF8_RC "${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8-executable.rc")
   set_source_files_properties("${MULLE_DEMO_UTF8_RC}" PROPERTIES
      COMPILE_OPTIONS "--include-dir=${CMAKE_CURRENT_SOURCE_DIR}/cmake")

   function(mulle_demo_utf8_argv)
      foreach(target IN LISTS ARGN)
         target_sources(${target} PRIVATE "${MULLE_DEMO_UTF8_RC}")
      endforeach()
   endfunction()

   mulle_demo_utf8_argv(multigrep logtop mredis mredis-stress fake-redis-server
                        urltoolkit windowrb hashspace)
endif()
