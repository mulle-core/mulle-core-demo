# cmake/ — the demo harness

Four files hold the pieces every demo would otherwise spell out again, and one
platform detail that is easy to get wrong. They belong to *this* project, not to
`mulle-core`: they are included and consumed by the top-level `CMakeLists.txt`
and by the demos' tests, and a fetch or a checkout of the library changes none of
them.

| file | what it is | where it is reached from |
|---|---|---|
| [cross-emulator.cmake](cross-emulator.cmake) | what runs a cross build's programs | `CMakeLists.txt` and every `test/run.cmake` |
| [mulle-demo.cmake](mulle-demo.cmake) | the warning flags and the `add_test()` block | `CMakeLists.txt` only |
| [utf8-executable.manifest](utf8-executable.manifest) | declares the process code page UTF-8 | compiled in via the `.rc` below |
| [utf8-executable.rc](utf8-executable.rc) | embeds that manifest as the process manifest | `CMakeLists.txt` on Windows |

## cross-emulator.cmake

`ctest` runs on the host, but a cross build's programs are built for another
system and cannot simply be executed there. CMake's own answer,
`CMAKE_CROSSCOMPILING_EMULATOR`, reaches `add_test(COMMAND <target>)`; these
tests are not that — each is a `cmake -P` script handed a program *path* and run
itself — so the emulator has to travel into the script. This file is both ends of
that journey:

- `mulle_demo_emulator()` — configure time, called once from the top level. It
  settles on `MULLE_DEMO_EMULATOR`: an explicit `-D` wins, then the toolchain's
  `CMAKE_CROSSCOMPILING_EMULATOR`, else `wine` for a Windows cross build, else a
  warning. It does nothing on a native build.
- `demo_program( VAR)` — script time, called from each `test/run.cmake`. It
  prefixes a program variable with the emulator, so a test runs the target-system
  binary through whatever runs it. It is a no-op when `EMULATOR` is undefined,
  which is what happens on every native build and when a test is run by hand.

The top level includes it at `CMakeLists.txt:166`; each test includes it from its
own directory by relative path.

## mulle-demo.cmake

The two things every demo's `CMakeLists.txt` would otherwise repeat, each with a
detail that is easy to drop silently:

- `mulle_demo_warnings( <target>...)` — applies `-Wall -Wextra` to a target
  (skipped for MSVC, which spells the flags differently). A target that misses it
  still compiles, just not like its neighbours.
- `mulle_demo_test( <name> ...)` — registers a demo's `test/run.cmake` with
  `add_test()`, handing it the programs and arguments the script asks for:
  `FIXTURES`, `PROGRAM <var> <target>` (`-D<var>=$<TARGET_FILE:target>`), and
  `ARG <var> <value>`. It always adds `WORKDIR` and the `EMULATOR` from
  `cross-emulator.cmake`, which is why `mulle_demo_emulator()` has to run first.
  A test that misses the emulator fails only in a cross build, and looks like the
  program's fault.

Included once at `CMakeLists.txt:173`, before the demos are added, so both
functions exist for all of them.

## utf8-executable.manifest and utf8-executable.rc

On Windows the C runtime builds `argv` by converting the wide command line to the
process ANSI code page (the system locale's, e.g. CP1252), so a program that
reads `argv` as UTF-8 receives mojibake for any non-ASCII argument, and the
original bytes are gone before `main()` runs. The manifest declares the process
code page as UTF-8 (Windows 10 1903 and later; older systems ignore it), and the
`.rc` embeds it as resource id 1, type 24 — `CREATEPROCESS_MANIFEST_RESOURCE_ID`
and `RT_MANIFEST` spelled out so any `windres`, llvm-windres included, compiles
it.

These two are inert on non-Windows builds. The WIN32 block at
`CMakeLists.txt:205-219` enables the `RC` language, points the resource at the
manifest, and applies it to each executable through `mulle_demo_utf8_argv()`.
That is a property of an executable, not of something it links, so it is a
function over a target list rather than an `INTERFACE` library.