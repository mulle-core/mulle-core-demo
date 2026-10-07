add_executable(urltoolkit urltoolkit.c)

target_link_libraries(urltoolkit PRIVATE mulle-core)

mulle_demo_warnings(urltoolkit)

mulle_demo_test(urltoolkit
   PROGRAM URLTOOLKIT urltoolkit
   FIXTURES)
