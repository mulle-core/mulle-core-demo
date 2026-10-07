add_executable(windowrb windowrb.c)

target_link_libraries(windowrb PRIVATE mulle-core)

# math for sqrt/log in the normal generator; on Windows this is part of the CRT
if(NOT MSVC)
   target_link_libraries(windowrb PRIVATE m)
endif()

mulle_demo_warnings(windowrb)

mulle_demo_test(windowrb
   PROGRAM WINDOWRB windowrb
   FIXTURES)
