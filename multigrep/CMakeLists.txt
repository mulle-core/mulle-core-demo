add_executable(multigrep multigrep.c)

target_link_libraries(multigrep PRIVATE mulle-core)

# memmem() is a GNU extension on glibc, declared only under _GNU_SOURCE, and a
# plain libc function on macOS and the BSDs. Windows has neither, and searches
# with the program's own prefilter instead -- see search_literal().
include(CheckSymbolExists)

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(memmem "string.h" MULLE_DEMO_HAVE_MEMMEM)
unset(CMAKE_REQUIRED_DEFINITIONS)

if(MULLE_DEMO_HAVE_MEMMEM)
   target_compile_definitions(multigrep PRIVATE MULLE_DEMO_HAVE_MEMMEM=1)
endif()

mulle_demo_warnings(multigrep)

mulle_demo_test(multigrep
   PROGRAM MULTIGREP multigrep
   FIXTURES)
