add_executable(logtop logtop.c)

target_link_libraries(logtop PRIVATE mulle-core)

mulle_demo_warnings(logtop)

mulle_demo_test(logtop
   PROGRAM LOGTOP logtop
   FIXTURES)
