# miniredis: the server, and the harness that proves things about it.
#
# The harness is also the RESP client used by the tests, so the test suite needs
# no redis-cli, no nc and no python.
add_executable(mredis
   mredis.c
   resp.c
   store.c)

add_executable(mredis-stress
   stress.c
   bench.c
   client.c
   resp.c
   store.c)

#
# The stand-in for a real redis-server, which `--compare` starts in place of one:
# mredis with redis-server's command line, in this same process. See the file for
# why it cannot be a shell script or a wrapper -- the short version is that the
# benchmark measures the pid it started, so there may only be one process.
#
add_executable(fake-redis-server
   fake-redis-server.c
   mredis.c
   resp.c
   store.c)

target_compile_definitions(fake-redis-server PRIVATE MREDIS_FIXTURE_MAIN=1)

foreach(target mredis mredis-stress fake-redis-server)
   target_link_libraries(${target} PRIVATE mulle-core
                                        PRIVATE Threads::Threads)
   mulle_demo_warnings(${target})
endforeach()

if(WIN32)
   #
   # winsock is mulle-core's line to draw; these two are the benchmark's own:
   # GetExtendedTcpTable() out of iphlpapi, GetProcessMemoryInfo() out of psapi.
   #
   target_link_libraries(mredis-stress PRIVATE iphlpapi psapi)
endif()

#
# SYSTEM_NAME is passed in because `cmake -P` has no CMAKE_SYSTEM_NAME of its
# own, and one assertion is about /proc: the target platform is what decides
# whether the binary under test can be asked that question. (The host's name is
# the wrong answer under cross-compilation, which is why it is spelled out here
# rather than left to CMAKE_HOST_SYSTEM_NAME in the script.)
#
# HAVE_ACCOUNTING is a capability and not a platform: whether the benchmark can
# ask the kernel about a server it started, which is /proc on Linux, libproc on
# macOS, and the process handle on Windows. client.h draws the same line with
# MREDIS_HAVE_ACCOUNTING; this is the test script's copy of it. Its absence is a
# named skip rather than a failure.
#
# There is no HAVE_POSIX flag here any more, and no HAVE_SOCKETS. Networking is
# mulle-socket's job on every platform, so the tcp half of the fixtures runs
# everywhere the rest does, and a flag that can only ever be 1 is a flag that
# lies about where the portability is. The benchmark was the last thing standing
# behind one, which is why it skipped on Windows while the rest did not.
#
if(WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
   set( MINIREDIS_HAVE_ACCOUNTING 1)
else()
   set( MINIREDIS_HAVE_ACCOUNTING 0)
endif()

#
# There is one test and it has three programs in it: the harness, the server it
# starts, and the stand-in server it starts in place of a real one. All three
# travel to the script as paths, because the harness spawns the servers itself --
# see mulle_demo_test() in cmake/mulle-demo.cmake.
#
mulle_demo_test(miniredis
   FIXTURES
   PROGRAM SERVER mredis
   PROGRAM STRESS mredis-stress
   PROGRAM STAND_IN fake-redis-server
   ARG SYSTEM_NAME ${CMAKE_SYSTEM_NAME}
   ARG HAVE_ACCOUNTING ${MINIREDIS_HAVE_ACCOUNTING})
