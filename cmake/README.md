# cmake/ — demo harness

Shared by the top-level `CMakeLists.txt` and the demos' tests. Belongs to this
project, not to `mulle-core`; a fetch or a checkout of the library changes none
of it.

| file | what it does | reached from |
|---|---|---|
| `cross-emulator.cmake` | runs a cross build's programs | `CMakeLists.txt`, every `test/run.cmake` |
| `mulle-demo.cmake` | warning flags + `add_test()` block | `CMakeLists.txt` only |
| `utf8-executable.manifest` | declares the process code page UTF-8 | embedded via the `.rc` |
| `utf8-executable.rc` | embeds that manifest as resource id 1, type 24 | `CMakeLists.txt` (Windows) |

## cross-emulator.cmake

The tests are `cmake -P` scripts handed a program *path*, not
`add_test(COMMAND <target>)`, so `CMAKE_CROSSCOMPILING_EMULATOR` cannot reach
them. This file carries it in:

- `mulle_demo_emulator()` — configure time, called once from the top level. Picks
  `MULLE_DEMO_EMULATOR`: explicit `-D`, then `CMAKE_CROSSCOMPILING_EMULATOR`,
  then `wine` for a Windows cross build, else warns. No-op on native builds.
- `demo_program( VAR)` — script time, called from each `test/run.cmake`. Prefixes
  a program variable with the emulator. No-op when `EMULATOR` is undefined
  (native build, or a test run by hand).

Included at `CMakeLists.txt:166` and relatively from each test.

## mulle-demo.cmake

- `mulle_demo_warnings( <target>...)` — `-Wall -Wextra` (skipped for MSVC).
- `mulle_demo_test( <name> ...)` — registers a demo's `test/run.cmake` with
  `add_test()`. Keywords: `FIXTURES`, `PROGRAM <var> <target>`,
  `ARG <var> <value>`. Always adds `WORKDIR` and `EMULATOR`, so
  `mulle_demo_emulator()` must have run first.

Included once at `CMakeLists.txt:173`, before the demos.

## utf8-executable.manifest / .rc

Windows builds `argv` by converting the wide command line to the process ANSI
code page, so UTF-8 arguments arrive as mojibake. The manifest declares the
process code page UTF-8 (Windows 10 1903+; older systems ignore it). The `.rc`
embeds it as resource id 1, type 24 (`CREATEPROCESS_MANIFEST_RESOURCE_ID` /
`RT_MANIFEST` spelled out for any windres). Inert off Windows; applied in the
WIN32 block at `CMakeLists.txt:205-219`.
