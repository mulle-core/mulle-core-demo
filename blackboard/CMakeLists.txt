#
# The one demo that is not just mulle-core: mulle-mmapallocator is a sibling
# project (it is deliberately not part of the amalgamation), so its one source
# file is compiled into this target with the defines its own CMakeLists.txt
# marks PUBLIC.
#
# mulle-mmapallocator's own build stages its headers under
# <include>/mulle-mmapallocator/ via mulle-sde. In a plain source checkout they
# sit flat in src/ next to src/generic (include.h) and src/reflect, so the
# include directories are spelled out here instead.
#
set(MULLE_MMAPALLOCATOR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../mulle-mmapallocator"
    CACHE PATH "Path to a mulle-mmapallocator source checkout")

set(_mmapallocator_dir "${MULLE_MMAPALLOCATOR_SOURCE_DIR}")

if(NOT EXISTS "${_mmapallocator_dir}/src/mulle-mmapallocator.c")
   if(NOT MULLE_DEMO_FETCH)
      message(WARNING
         "No mulle-mmapallocator checkout at '${MULLE_MMAPALLOCATOR_SOURCE_DIR}', "
         "and MULLE_DEMO_FETCH is off.\n"
         "The blackboard demo needs it; reconfigure with "
         "-DMULLE_MMAPALLOCATOR_SOURCE_DIR=/path/to/mulle-mmapallocator")
      return()
   endif()

   include(FetchContent)
   FetchContent_Declare(mulle-mmapallocator
      GIT_REPOSITORY "${MULLE_DEMO_GIT_BASE}/mulle-mmapallocator.git"
      ${_mulle_demo_fetch_tag}
      GIT_SHALLOW TRUE
      SOURCE_SUBDIR _mulle_demo_populate_only)   # get it, do not add_subdirectory it
   FetchContent_MakeAvailable(mulle-mmapallocator)

   set(_mmapallocator_dir "${mulle-mmapallocator_SOURCE_DIR}")
   message(STATUS "Using fetched mulle-mmapallocator: ${_mmapallocator_dir}")
endif()

set(_mmapallocator_src "${_mmapallocator_dir}/src")

#
# mulle-mmapallocator's source includes "mulle-dlmalloc/src/dlmalloc.c", and
# mulle-dlmalloc is a mulle-sde *fetched* dependency of that project: mulle-sde
# clones it into src/mulle-dlmalloc of the checkout, and it is not tracked, so a
# checkout that was never crafted with mulle-sde does not have it -- which is
# every fresh clone. It is one file, so it is fetched on its own and presented
# under the name the include spells, in this build tree rather than written into
# anyone's checkout.
#
set(_mmapallocator_dlmalloc "${_mmapallocator_src}/mulle-dlmalloc/src/dlmalloc.c")
set(_mmapallocator_dlmalloc_root "")

if(NOT EXISTS "${_mmapallocator_dlmalloc}")
   if(NOT MULLE_DEMO_FETCH)
      message(WARNING
         "mulle-mmapallocator has no '${_mmapallocator_dlmalloc}', and "
         "MULLE_DEMO_FETCH is off.\n"
         "Its source includes that file, and it arrives by way of "
         "`mulle-sde fetch` in that checkout. Craft it there, or reconfigure "
         "with -DMULLE_DEMO_FETCH=ON")
      return()
   endif()

   include(FetchContent)
   FetchContent_Declare(mulle-dlmalloc
      GIT_REPOSITORY "${MULLE_DEMO_GIT_BASE}/mulle-dlmalloc.git"
      ${_mulle_demo_fetch_tag}
      GIT_SHALLOW TRUE
      SOURCE_SUBDIR _mulle_demo_populate_only)
   FetchContent_MakeAvailable(mulle-dlmalloc)

   set(_mmapallocator_dlmalloc_root "${CMAKE_CURRENT_BINARY_DIR}/fetched")
   file(COPY "${mulle-dlmalloc_SOURCE_DIR}/src/dlmalloc.c"
        DESTINATION "${_mmapallocator_dlmalloc_root}/mulle-dlmalloc/src")
   message(STATUS "Using fetched mulle-dlmalloc: ${mulle-dlmalloc_SOURCE_DIR}")
endif()

add_executable(blackboard
   blackboard.c
   "${_mmapallocator_src}/mulle-mmapallocator.c")

target_compile_definitions(blackboard PRIVATE
   MSPACES
   ONLY_MSPACES
   NO_MALLINFO)

target_include_directories(blackboard PRIVATE
   "${_mmapallocator_src}"            # mulle-mmapallocator.h - flat in a checkout
   "${_mmapallocator_src}/generic"    # include.h / include-private.h
   "${_mmapallocator_src}/reflect"    # _mulle-mmapallocator-include.h
   ${_mmapallocator_dlmalloc_root})    # unquoted: nothing at all when it was not needed

target_link_libraries(blackboard PRIVATE mulle-core)

# blackboard.c gets the same -Wall -Wextra as the other demos. The sibling source
# is silenced at the file level instead: it includes dlmalloc, whose unused
# helpers would otherwise bury the demo's own diagnostics.
set_source_files_properties("${_mmapallocator_src}/mulle-mmapallocator.c"
   PROPERTIES COMPILE_OPTIONS "-w")

mulle_demo_warnings(blackboard)

mulle_demo_test(blackboard
   PROGRAM BLACKBOARD blackboard
   ARG SYSTEM_NAME ${CMAKE_SYSTEM_NAME})
