#
# Nothing but mulle-core: mulle-concurrent is part of the amalgamation, reached
# through <mulle-core/mulle-core.h>.
#
add_executable(hashspace
   hashspace.c)

target_link_libraries(hashspace PRIVATE mulle-core)

mulle_demo_warnings(hashspace)

#
# Two of the assertions are about what happens with a bad table size, and a
# build with assertions live fails those earlier: the storage allocator's assert
# fires before any of it. So the test has to know which of the two it is
# testing, and the build type is a poor proxy for that -- an empty
# CMAKE_BUILD_TYPE (CMake's default) has the assertions live but is not "Debug",
# so treating anything but Debug as an NDEBUG build gets it backwards.
# The flags that will actually build the target are the honest answer. Asking
# try_compile instead does not work: it does not inherit CMAKE_C_FLAGS_<CONFIG>,
# so it reports assertions live even in a Release build.
#
# the per configuration variables are uppercased, however the build type is spelled
#
string(TOUPPER "${CMAKE_BUILD_TYPE}" HASHSPACE_BUILD_TYPE)

set( HASHSPACE_ASSERTIONS ON)
foreach(flag IN LISTS CMAKE_C_FLAGS CMAKE_C_FLAGS_${HASHSPACE_BUILD_TYPE})
   if(flag MATCHES "(^| )-DNDEBUG(=[^ ]*)?( |$)")
      set( HASHSPACE_ASSERTIONS OFF)
   endif()
endforeach()

mulle_demo_test(hashspace
   PROGRAM HASHSPACE hashspace
   ARG ASSERTIONS ${HASHSPACE_ASSERTIONS})
