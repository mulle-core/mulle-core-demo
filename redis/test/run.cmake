#
# miniredis tests.
#
# Two kinds of assertion, and they are deliberately different in kind:
#
#  * the keyspace modes are *measurements*. --concurrency asserts the real
#    keyspace never loses a key, and --concurrency --naive asserts that the
#    obvious alternative does. If the naive one ever stops losing keys, this
#    test fails, and that is the point: it would mean the premise of
#    wontfix/11-no-atomic-replace.md (declined -- there is no replace by design)
#    needs rechecking rather than the test relaxing.
#
#  * the script fixtures are exact comparisons of decoded replies, run twice:
#    once over a stdio child and once over a real socket. Identical output from
#    two transports is what says the network path adds nothing of its own.
#

# A cross build's harness cannot be run by this host, so it is run through an
# emulator when there is one, and unquoted at the exec sites: STRESS may be two
# words. SERVER is left alone at those sites -- the harness spawns the server
# itself, so what it wants is a path, not a command.
#
# The two shell scripts further down start the server themselves and then talk to
# it, so there both programs have to be the command. They use the *string* form
# of it, because what a shell is handed is text: a CMake list is ';' separated
# and a ';' inside a shell script is a command separator. That leaves the one
# thing unquoted expansion cannot do, which is a path with a space in it --
# neither program has one, and quoting them would collapse the emulator and the
# program back into one word wherever there is an emulator to collapse.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/cross-emulator.cmake")
demo_program( STRESS)

# The command forms, for the shell scripts below. SERVER is copied before it is
# wrapped, because elsewhere it is a *path*: `--server PATH` is the harness being
# told it will spawn the server itself, so an emulator in front of that argument
# would arrive as a second argument.
set( STRESS_COMMAND "${STRESS}")
set( SERVER_COMMAND "${SERVER}")
demo_program( SERVER_COMMAND)

foreach(command STRESS_COMMAND SERVER_COMMAND)
   string(REPLACE ";" " " ${command} "${${command}}")
endforeach()


function(require_match name actual pattern)
   if(NOT "${actual}" MATCHES "${pattern}")
      message(FATAL_ERROR "${name}: expected /${pattern}/, got:\n${actual}")
   endif()
endfunction()

function(require_no_match name actual pattern)
   if("${actual}" MATCHES "${pattern}")
      message(FATAL_ERROR "${name}: did not expect /${pattern}/, got:\n${actual}")
   endif()
endfunction()

function(run_harness name)
   execute_process(COMMAND ${STRESS} ${ARGN}
                   RESULT_VARIABLE rval
                   OUTPUT_VARIABLE out
                   ERROR_VARIABLE err)
   if(rval)
      message(FATAL_ERROR "${name}: the harness failed (${rval})\n${out}${err}")
   endif()
   set(${name}_out "${out}" PARENT_SCOPE)
endfunction()


# ---------------------------------------------------------------- reclamation
#
# Replaced values have to be freed. Getting this wrong is a use-after-free
# rather than a wrong answer, so it is worth its own mode.

run_harness(reclaim --reclaim)
message(STATUS "${reclaim_out}")
require_match("reclaim" "${reclaim_out}" "uncontended writes=2000 live=1 retired=0")
require_match("reclaim" "${reclaim_out}" "after_delete live=0 keys=0")
require_match("reclaim" "${reclaim_out}" "after_churn ops=20000 live=1 retired=0")

# ------------------------------------------------------------------ collisions
#
# mulle_concurrent_hashtable holds exactly one value per hash: 8 bits of hash
# and 2000 keys guarantees collisions. Detecting them is our job, and aliasing
# one key onto another's value is the failure mode -- silent, and it would look
# like a working server.

run_harness(collision --collision)
message(STATUS "${collision_out}")
require_match("collision" "${collision_out}" "aliased=0")
require_match("collision" "${collision_out}" "errors=0")
require_no_match("collision" "${collision_out}" "collisions=0")

# ---------------------------------------------------------------- concurrency
#
# The key exists before any thread starts and is never deleted, so a reader
# seeing it absent is a linearizability violation, full stop.

run_harness(entry --concurrency --threads 8 --ops 50000)
message(STATUS "${entry_out}")
require_match("entry" "${entry_out}" "mode=entry")
require_match("entry" "${entry_out}" "missing=0")

# The same workload against SET spelled remove-then-insert. Note --no-yield:
# there is no artificial delay inside the window here, so what this measures is
# the window itself, not a window we widened.
run_harness(naive --concurrency --naive --no-yield --threads 8 --ops 50000)
message(STATUS "${naive_out}")
require_match("naive" "${naive_out}" "mode=naive")
require_no_match("naive" "${naive_out}" "missing=0")

# -------------------------------------------------------------------- fixtures

foreach(fixture basic inline binary protocol)
   foreach(transport stdio tcp)
      if(transport STREQUAL "tcp")
         set(flag --tcp)
      else()
         set(flag)
      endif()

      execute_process(COMMAND ${STRESS} --server ${SERVER} ${flag}
                              --script ${FIXTURES}/${fixture}.txt
                      RESULT_VARIABLE rval
                      OUTPUT_VARIABLE out
                      ERROR_VARIABLE err)
      if(rval)
         message(FATAL_ERROR "${fixture}/${transport}: exit ${rval}\n${out}${err}")
      endif()

      #
      # A Windows CRT writes \r\n for every \n the program printed, so undo
      # exactly that before comparing: the golden files are written with LF, and
      # it is the only difference there is. Nothing in a report is a deliberate
      # CR -- non printable bytes of the protocol come through as \xNN, which is
      # what lets the comparison stay byte for byte.
      #
      string(REPLACE "\r\n" "\n" out "${out}")

      file(READ ${FIXTURES}/${fixture}.expected expected)
      if(NOT out STREQUAL expected)
         message(FATAL_ERROR
            "${fixture}/${transport}: replies differ\n--- expected\n${expected}--- got\n${out}")
      endif()
      message(STATUS "${fixture}/${transport}: ok")
   endforeach()
endforeach()

# ----------------------------------------------------------------------- bench
#
# The benchmark is short here and asserts structure, not speed: a rate that
# depends on the machine would make this test flaky, while "the counter moved",
# "no reply was wrong" and "the kernel reported an RSS" are properties of the
# program. errors=0 is the load bearing one -- it means every SET came back OK
# and every GET came back with a value of exactly the size that was written.

function(run_bench name)
   execute_process(COMMAND ${STRESS} --server ${SERVER} --bench --quiet ${ARGN}
                   RESULT_VARIABLE rval
                   OUTPUT_VARIABLE out
                   ERROR_VARIABLE err)
   if(rval)
      message(FATAL_ERROR "${name}: the benchmark failed (${rval})\n${out}${err}")
   endif()
   set(${name}_out "${out}" PARENT_SCOPE)
   set(${name}_err "${err}" PARENT_SCOPE)
endfunction()

#
# The benchmark reads the server's memory and CPU out of the kernel -- /proc on
# Linux, libproc on macOS, the process handle on Windows. Without one of those
# the harness refuses --bench rather than report numbers it made up, and its
# refusal is not a test failure. The bench is the last thing this script does, so
# the rest of it is what gets skipped.
#
if(NOT HAVE_ACCOUNTING)
   message(STATUS "bench: skipped, no process accounting on ${SYSTEM_NAME}")
   return()
endif()

run_bench(bench_tcp --threads 4 --seconds 0.5)
message(STATUS "${bench_tcp_out}")
require_match("bench threads" "${bench_tcp_out}" "transport=tcp threads=4")
require_match("bench errors" "${bench_tcp_out}" "errors=0")
# the counters have to move, and the kernel has to answer for the server
require_no_match("bench requests" "${bench_tcp_out}" " requests=0 ")
require_match("bench rss" "${bench_tcp_out}" "server_rss_kb=[1-9][0-9]*")
require_match("bench rate" "${bench_tcp_out}" "req_per_s=[1-9][0-9]*\\.[0-9]")
# Both CPU readings come from the kernel -- the server's utime+stime out of
# /proc/<pid>/stat on Linux or proc_pid_rusage() on macOS, the client's out of
# getrusage() on either. Either one reading 0 means
# the attribution is unavailable, which is a silent "no idea who is the limit".
require_match("bench server cpu" "${bench_tcp_out}" "server_cpu_ms=[1-9][0-9]*")
require_match("bench client cpu" "${bench_tcp_out}" "client_cpu_ms=[1-9][0-9]*")

# over a pipe, and with the thread count the pipe can actually support
run_bench(bench_stdio --threads 4 --seconds 0.5 --stdio)
message(STATUS "${bench_stdio_out}")
require_match("bench stdio" "${bench_stdio_out}" "transport=stdio threads=1")
require_match("bench stdio errors" "${bench_stdio_out}" "errors=0")
require_match("bench stdio warns" "${bench_stdio_err}" "threads forced to 1")

# a bigger payload has to keep working: this is the path where the value no
# longer fits one wire buffer
run_bench(bench_big --threads 2 --seconds 0.5 --value-size 4096 --keys 64)
message(STATUS "${bench_big_out}")
require_match("bench big" "${bench_big_out}" "value_size=4096")
require_match("bench big errors" "${bench_big_out}" "errors=0")

# The same 4 KB payload, with the *shape* of the latency as the assertion.
#
# The test above was green the whole time this was broken, and that is the point
# of this one: errors=0 says the replies were right, not that they were prompt.
# mredis accepted connections without TCP_NODELAY, so Nagle held the partial
# segment that ends every reply batch until the peer's delayed ACK -- a flat ~40 ms
# per round trip whatever the machine or thread count, which at pipeline 16 is
# 2.4 ms of p50 per request instead of 5 us.
#
# So the assertion is a ratio between two numbers this machine produced for
# itself, and it does not care how fast the machine is. With the flag set, 4 KB
# values cost a few microseconds per request and the ratio is near 1; without it
# the ratio is the delayed-ACK timer over a microsecond, in the thousands.
# Measured here: 1.6 with the flag, 1569 without. The limit of 50 is about 30x
# away from each, so it discriminates without being a stopwatch.
run_bench(bench_lat_small --threads 1 --seconds 0.5 --pipeline 16)
run_bench(bench_lat_4k --threads 1 --seconds 0.5 --pipeline 16 --value-size 4096 --keys 64)
message(STATUS "${bench_lat_small_out}")
message(STATUS "${bench_lat_4k_out}")
require_match("bench latency 4k" "${bench_lat_4k_out}" "pipeline=16 value_size=4096")
require_match("bench latency 4k errors" "${bench_lat_4k_out}" "errors=0")
string(REGEX MATCH "p50_ns=([0-9]+)" ignore_small "${bench_lat_small_out}")
set(lat_small "${CMAKE_MATCH_1}")
string(REGEX MATCH "p50_ns=([0-9]+)" ignore_4k "${bench_lat_4k_out}")
set(lat_4k "${CMAKE_MATCH_1}")
if(lat_small EQUAL 0 OR lat_4k EQUAL 0)
   message(FATAL_ERROR "bench latency: no p50 in the summary (64 B='${lat_small}' 4096 B='${lat_4k}')\n${bench_lat_small_out}${bench_lat_4k_out}")
endif()
math(EXPR lat_ratio "${lat_4k} / ${lat_small}")
if(lat_ratio GREATER 50)
   message(FATAL_ERROR "bench latency: 4096 B values cost ${lat_ratio}x the per-request latency of 64 B values (p50 ${lat_4k} ns against ${lat_small} ns, pipeline 16). That flat multiple is a TCP timer rather than work: check that mredis sets TCP_NODELAY on accepted sockets (and that the client sets it on the sockets it connects), because without it Nagle holds the tail of every batch until the peer's delayed ACK.")
endif()
message(STATUS "bench latency: p50 ${lat_4k} ns at 4096 B against ${lat_small} ns at 64 B, ratio ${lat_ratio} (allowed < 50)")

# A batch larger than one read on the server side. 256 commands of ~98 bytes is
# just over the 16 KB the server reads at a time, so a command straddles the
# boundary -- which is where the parser used to call a valid batch a protocol
# error. Keep it here: this is the smallest pipelining case that covers it.
run_bench(bench_boundary --threads 1 --seconds 0.5 --pipeline 256 --keys 256)
message(STATUS "${bench_boundary_out}")
require_match("bench boundary" "${bench_boundary_out}" "pipeline=256")
require_match("bench boundary errors" "${bench_boundary_out}" "errors=0")
require_no_match("bench boundary" "${bench_boundary_out}" "requests=0 ")

# --compare runs the same workload against two servers and prints the pair
# side by side. The second server is the `fake-redis-server` target, which takes
# redis-server's command line (and rejects anything it would reject) and then
# serves with mredis, in the same process -- so this covers the spawning, the
# port the second server picked, and the summary, on a machine with no redis
# installed. Pointing --redis at a real redis-server is the same code path with
# real numbers.
execute_process(COMMAND ${STRESS} --server ${SERVER} --bench --compare
                        --redis ${STAND_IN}
                        --threads 2 --seconds 0.5
                RESULT_VARIABLE compare_rval
                OUTPUT_VARIABLE compare_out
                ERROR_VARIABLE compare_err)
if(compare_rval)
   message(FATAL_ERROR "bench compare: failed (${compare_rval})\n${compare_out}${compare_err}")
endif()
message(STATUS "${compare_out}")
require_match("bench compare pairs"  "${compare_out}" "a=mredis-spawned b=redis-spawned")
require_match("bench compare errors" "${compare_out}" "a_errors=0 b_errors=0")
# both halves have to have moved, and both RSS values have to be real
require_no_match("bench compare a"   "${compare_out}" "a_req_per_s=0")
require_no_match("bench compare b"   "${compare_out}" "b_req_per_s=0")
require_match("bench compare rss"    "${compare_out}" "a_rss_kb=[1-9][0-9]* b_rss_kb=[1-9][0-9]*")
require_match("bench compare cpu"    "${compare_out}" "a_server_cpu_ms=[1-9][0-9]* b_server_cpu_ms=[1-9][0-9]*")

# The same comparison against a real redis-server, when one is installed. This is
# the only place the suite talks to a third-party server, so it is skipped rather
# than required -- the stand-in above is what keeps the path covered everywhere,
# and this is what proves the stand-in is telling the truth when it can. No rates
# are asserted: they are the machine's, not the program's.
find_program(REDIS_SERVER redis-server)
if(REDIS_SERVER AND EMULATOR)
   #
   # A cross build finds the *host's* redis-server, and that is not a program
   # this target can start and then stop. wine will happily run the ELF, which is
   # worse than refusing: the comparison runs and the harness then hangs trying to
   # end it, leaving the server behind. So this one wants both -- a redis-server,
   # and a build whose programs start natively.
   #
   message(STATUS "bench compare redis: skipped, the host's redis-server is not a"
                  " ${SYSTEM_NAME} program")
elseif(REDIS_SERVER)
   execute_process(COMMAND ${STRESS} --server ${SERVER} --bench --compare
                           --redis ${REDIS_SERVER} --threads 2 --seconds 0.5
                   RESULT_VARIABLE redis_rval
                   OUTPUT_VARIABLE redis_out
                   ERROR_VARIABLE redis_err)
   if(redis_rval)
      message(FATAL_ERROR "bench compare redis: failed (${redis_rval})\n${redis_out}${redis_err}")
   endif()
   message(STATUS "${redis_out}")
   require_match("bench compare redis"  "${redis_out}" "compare: a=mredis-spawned b=redis-spawned")
   require_match("bench compare redis errors" "${redis_out}" "a_errors=0 b_errors=0")
   require_match("bench compare redis rss"    "${redis_out}" "b_rss_kb=[1-9][0-9]*")
   require_match("bench compare redis cpu"    "${redis_out}" "b_server_cpu_ms=[1-9][0-9]*")
else()
   message(STATUS "bench compare redis: skipped, no redis-server in PATH")
endif()

# --connect benches a server that is already running, which is the only path
# that has to resolve a port to a pid in order to report RSS (/proc on Linux,
# libproc on macOS, the TCP table on Windows).
# That lookup is one field-count away from reading the `timeout` column instead
# of the `inode` column -- and `timeout` is 0 for a listener, so the bug looks
# like "RSS is not available" rather than like a bug. Assert the kernel answered.
set(connect_script "
${SERVER_COMMAND} --port 0 --bind 127.0.0.1 2>\"${WORKDIR}/connect.banner\" &
server=\$!
port=
for i in 1 2 3 4 5 6 7 8 9 10; do
   port=\$(sed -n 's/.*listening on 127.0.0.1:\\([0-9]*\\).*/\\1/p' \"${WORKDIR}/connect.banner\")
   [ -n \"\$port\" ] && break
   sleep 0.2
done
if [ -z \"\$port\" ]; then
   kill \$server 2>/dev/null
   echo 'no banner with a port' >&2
   exit 1
fi
${STRESS_COMMAND} --bench --connect 127.0.0.1:\$port --threads 2 --seconds 0.5 --quiet
rval=\$?
kill \$server 2>/dev/null
exit \$rval
")
execute_process(COMMAND sh -c "${connect_script}"
                RESULT_VARIABLE connect_rval
                OUTPUT_VARIABLE connect_out
                ERROR_VARIABLE connect_err)
if(connect_rval)
   message(FATAL_ERROR "bench connect: failed (${connect_rval})\n${connect_out}${connect_err}")
endif()
message(STATUS "${connect_out}")
require_match("bench connect"      "${connect_out}" "server=connected")
require_match("bench connect err"  "${connect_out}" "errors=0")
require_match("bench connect rss"  "${connect_out}" "server_rss_kb=[1-9][0-9]*")

# The server's CPU reading is utime+stime out of /proc/<pid>/stat, and those two
# fields are adjacent: state ppid ... cmajflt | utime stime cutime cstime. Skip a
# field between them and you read cutime -- the *children's* user time, 0 for a
# server that forks nothing -- so the number stays plausible while quietly being
# user time only. That is 14% of the truth here, because this server's cost is
# mostly kernel time, and it is invisible from the summary line. So: read the
# same file the bench reads, bracket the run with our own two readings, and
# require the bench's number to be at least half of the independent total.
# Measured here: ratio 1.00 reading utime+stime, 0.14 reading user time only.
#
# Linux only, and unavoidably so: the test works by taking an *independent*
# reading of the very file the bench parses, and there is no /proc to read on
# macOS. process_cpu_seconds() itself is portable (proc_pid_rusage() there,
# /proc here) -- it is this cross-check of its /proc branch that cannot be.
if(SYSTEM_NAME STREQUAL "Linux")

set(cpu_script "
${SERVER_COMMAND} --port 0 --bind 127.0.0.1 2>\"${WORKDIR}/cpu.banner\" &
server=\$!
port=
for i in 1 2 3 4 5 6 7 8 9 10; do
   port=\$(sed -n 's/.*listening on 127.0.0.1:\\([0-9]*\\).*/\\1/p' \"${WORKDIR}/cpu.banner\")
   [ -n \"\$port\" ] && break
   sleep 0.2
done
if [ -z \"\$port\" ]; then
   kill \$server 2>/dev/null
   echo 'no banner with a port' >&2
   exit 1
fi
hz=\$(getconf CLK_TCK 2>/dev/null || echo 100)
# after the comm field: utime is field 12, stime 13 -- utime+stime is the whole
# process, every thread of it, which is what the bench claims to report.
ticks=\$(sed 's/^[^)]*) //' /proc/\$server/stat | awk '{print \$12+\$13}')
before=\$ticks
out=\$(${STRESS_COMMAND} --bench --connect 127.0.0.1:\$port --threads 16 --seconds 1 --quiet)
rval=\$?
after=\$(sed 's/^[^)]*) //' /proc/\$server/stat | awk '{print \$12+\$13}')
kill \$server 2>/dev/null
[ \$rval -eq 0 ] || { echo \"\$out\"; exit \$rval; }
measured=\$(echo \"\$out\" | sed -n 's/.*server_cpu_ms=\\([0-9]*\\).*/\\1/p')
[ -n \"\$measured\" ] || { echo 'no server_cpu_ms in the summary' >&2; exit 1; }
total_ms=\$(( (after - before) * 1000 / hz ))
if [ \$(( measured * 2 )) -lt \$total_ms ]; then
   echo \"server_cpu_ms=\$measured is under half of the independent reading \$total_ms ms: the bench is\" >&2
   echo 'not counting the server. Check the utime/stime field skip in process_cpu_seconds().' >&2
   exit 1
fi
echo \"\$out\"
echo \"cpu attribution: server_cpu_ms=\$measured independent_ms=\$total_ms\"
")
execute_process(COMMAND sh -c "${cpu_script}"
                RESULT_VARIABLE cpu_rval
                OUTPUT_VARIABLE cpu_out
                ERROR_VARIABLE cpu_err)
if(cpu_rval)
   message(FATAL_ERROR "bench cpu attribution: failed (${cpu_rval})\n${cpu_out}${cpu_err}")
endif()
message(STATUS "${cpu_out}")
require_match("bench cpu attribution" "${cpu_out}" "cpu attribution: server_cpu_ms=[1-9][0-9]* independent_ms=[1-9][0-9]*")

else()

message(STATUS "bench cpu attribution: skipped, no /proc on ${SYSTEM_NAME}")

endif()
