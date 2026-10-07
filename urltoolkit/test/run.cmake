#
# Fixture tests for urltoolkit, run by ctest or directly:
#
#   cmake -DURLTOOLKIT=<binary> -DFIXTURES=<dir> -DWORKDIR=<dir> -P test/run.cmake
#
# What is asserted is behaviour that was measured on the checkout this was
# written against, not timing: which component rejects which codepoint, how many
# URLs the two implementations agree on, what the slug produces, and that every
# failure path fails. Several assertions are the findings themselves -- if
# mulle-url gains a percent triplet check or a port class, these tests will go
# red and the reports will need revisiting, which is the point.
#

foreach(required URLTOOLKIT FIXTURES WORKDIR)
   if(NOT DEFINED ${required})
      message(FATAL_ERROR "${required} is not defined")
   endif()
endforeach()

# A cross build's binary cannot be run by this host, so it is run through an
# emulator when there is one. Unquoted from here on: URLTOOLKIT may be two words.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/cross-emulator.cmake")
demo_program( URLTOOLKIT)


function(run NAME)
   execute_process(COMMAND ${URLTOOLKIT} ${ARGN}
                   WORKING_DIRECTORY "${WORKDIR}"
                   OUTPUT_VARIABLE out
                   ERROR_VARIABLE  err
                   RESULT_VARIABLE rc)
   set(${NAME}_out "${out}" PARENT_SCOPE)
   set(${NAME}_err "${err}" PARENT_SCOPE)
   set(${NAME}_rc  "${rc}"  PARENT_SCOPE)
endfunction()


function(require_match NAME HAYSTACK PATTERN)
   if(NOT "${HAYSTACK}" MATCHES "${PATTERN}")
      message("FAIL ${NAME}: no match for [${PATTERN}]\n  in: [${HAYSTACK}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


function(require_absent NAME HAYSTACK PATTERN)
   if("${HAYSTACK}" MATCHES "${PATTERN}")
      message("FAIL ${NAME}: unexpected match for [${PATTERN}]\n  in: [${HAYSTACK}]")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


function(require_exit NAME ACTUAL EXPECTED)
   if(NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
      message("FAIL ${NAME}: exit ${ACTUAL}, expected ${EXPECTED}")
      set_property(GLOBAL APPEND PROPERTY failures "${NAME}")
   else()
      message(STATUS "ok   ${NAME}")
   endif()
endfunction()


set(urls "${FIXTURES}/urls.txt")


# ------------------------------------------------------------------- validate

run(validate --validate "${urls}")

require_exit("validate exits 0" "${validate_rc}" "0")
require_match("validate counts every line" "${validate_out}"
   "urlvalidate: urls=35 ok=27 invalid=8 codepoints=717 pruned=2 ports=7 range=1")

# The findings, visible in the output rather than only in a report file.
#
# A percent triplet is not a per-codepoint question: the component classifiers
# reject '%' and mulle-url answers with the slice check, so the triplet is
# accepted here.
require_match("a percent triplet is accepted" "${validate_out}"
   "http://example.com/a%20b +ok +scheme=http host=example.com path=/a%20b")
#
# `;` is a `pchar` and mulle-url now agrees -- see fixed/23, which is why this
# row moved from INVALID ("path U+003B at codepoint 5") to ok. The three matrix
# parameter URLs in the corpus are what the counts above turned on.
# The `[;]` spelling keeps the literal out of a CMake list.
#
require_match("a matrix parameter is a valid pchar" "${validate_out}"
   "http://example.com/path[;]p=1 +ok +scheme=http host=example.com path=/path[;]p=1")
require_match("a non-ASCII path fails validation" "${validate_out}"
   "http://example.com/.* INVALID +path U\\+00FC at codepoint 1")
require_match("an astral codepoint is plane-pruned" "${validate_out}"
   "INVALID +path U\\+1F600 at codepoint 1")

# The port goes through mulle-url's port class like any other component, so a
# non-digit is rejected on the character, not by a demo-side numeric check.
require_match("a non-numeric port is rejected by mulle-url" "${validate_out}"
   "http://example.com:abc/ +INVALID +port U\\+0061 at codepoint 0")
# A port above 16 bits is still syntactically `*DIGIT`, so the demo notes it
# and does not call the URL invalid -- the range reading is the demo's.
require_match("an out-of-range port is a note, not a verdict" "${validate_out}"
   "http://example.com:99999/ +ok .*\\(port 99999 outside 0\\.\\.65535\\)")
require_match("ports are counted" "${validate_out}" "Ports seen +: 7")
require_match("the range note is counted apart" "${validate_out}"
   "Ports out of range: 1")

# A URL that everything agrees about, and the components it was split into.
require_match("userinfo is split for validation" "${validate_out}"
   "ok scheme=http user=user password=pass host=example.com port=8080 path=/a/b query=x=y")
require_match("an IPv6 authority keeps its brackets" "${validate_out}"
   "host=.*port=8080 path=/x")

# A relative reference has no authority and no scheme, and the demo does not
# invent one for it.
require_match("a relative reference stays a path" "${validate_out}"
   "www.example.com/page.html +ok path=www.example.com/page.html")


# ----------------------------------------------------------------- crosscheck

run(crosscheck --crosscheck "${urls}")
require_exit("crosscheck exits 0" "${crosscheck_rc}" "0")
require_match("crosscheck counts" "${crosscheck_out}"
   "urlcheck: urls=35 parser_ok=21 ours_ok=27 agree=25 verdict=7 structure=2 ambiguity=1 ports=7")

# The ambiguity: we read a host and a port, the parser reads a scheme.
require_match("the ambiguous URL is named" "${crosscheck_out}"
   "example.com:8080/path")
require_match("the parser's reading is reported" "${crosscheck_out}"
   "read the pre-colon part as a scheme")
require_match("the ambiguity category is explicit" "${crosscheck_out}"
   "disagree +host:port here, scheme there")

# Both accept, but see different components: a protocol-relative URL, and an
# empty fragment (present to us, absent to the parser).
require_match("protocol-relative differs in structure" "${crosscheck_out}"
   "same verdict, different components")
require_match("an empty fragment differs in structure" "${crosscheck_out}"
   "http://example.com/path#")

# Both accept the percent-encoded path now that the demo uses mulle-url's
# triplet check, so no codepoint of it is reported invalid.
require_absent("no percent triplet is rejected" "${crosscheck_out}" "U\\+0025")
# The remaining verdict disagreements are about scope or character policy: the
# parser is a request-target parser, mulle-url is RFC 3986. `|` is not a pchar,
# so mulle-url rejects a path the parser tolerates.
require_match("a bar in a path is a verdict disagreement" "${crosscheck_out}"
   "http://example.com/a\\|b")
require_match("the bar is rejected on the codepoint" "${crosscheck_out}"
   "U\\+007C at codepoint 2")
require_absent("no URL is both accepted and rejected silently" "${crosscheck_out}"
   "verdict +unknown")


# --------------------------------------------------------------------- fields

run(fields --fields "${urls}")
require_exit("fields exits 0" "${fields_rc}" "0")
require_match("fields prints the parser's components" "${fields_out}"
   "https://example.com/path.* parser schema host path query fragment")
require_match("fields prints a failure" "${fields_out}" "parser FAIL")


# ----------------------------------------------------------------------- slug

run(slug --slug "${FIXTURES}/titles.txt")
require_exit("slug exits 0" "${slug_rc}" "0")

# Latin-1 transliteration, in both flavors.
require_match("an umlaut is transliterated" "${slug_out}" "Uber-Grosse-und-Strasse")
require_match("an accent is transliterated" "${slug_out}" "naive-cafe-resume")
# A character with no Latin equivalent is dropped in one flavor and kept in the
# other, which is the whole reason both exist.
require_match("the ASCII slug drops what it cannot transliterate" "${slug_out}"
   "utf8 kept: ")
require_match("the utf8 slug keeps it" "${slug_out}"
   "utf8 kept: .+")
# The delimiter is a parameter.
require_match("a custom delimiter" "${slug_out}" "buffer, '_': Uber_Grosse_und_Strasse")
require_match("slug counts" "${slug_out}" "urlslug: lines=8")


# -------------------------------------------------------------------- extract

run(extract --extract "https?://[[:alnum:].%/-]+" "${FIXTURES}/access.log")
require_exit("extract exits 0" "${extract_rc}" "0")

# --extract and --rewrite are filters, not reports: the output is exactly the
# matched lines and no summary, so that they can be piped into something else.
# The second match is `http://user`, not the whole userinfo URL, because ':' is
# not in the class and the leftmost match wins -- worth pinning down, since a
# caller expecting PCRE semantics will be surprised by both halves of that.
set(extract_expected "https://example.com/a%20b
http://user
https://example.com/path
https://example.com/ü
https://example.com/submit
")
if("${extract_out}" STREQUAL "${extract_expected}")
   message(STATUS "ok   extract prints exactly the matches")
else()
   message("FAIL extract prints exactly the matches\n  got: [${extract_out}]")
   set_property(GLOBAL APPEND PROPERTY failures "extract output")
endif()
require_absent("extract prints no summary" "${extract_out}" "urlextract")

run(extract_pct --extract "%[0-9A-Fa-f][0-9A-Fa-f]" "${FIXTURES}/access.log")
require_match("a percent escape can be extracted" "${extract_pct_out}" "%20")


# -------------------------------------------------------------------- rewrite

run(rewrite --rewrite "http://" "https://" "${FIXTURES}/access.log")
require_exit("rewrite exits 0" "${rewrite_rc}" "0")

# One line had a plain http:// and was rewritten, the other four were already
# https:// and come through byte for byte -- prefix and suffix intact, which is
# the part the library leaves to the caller.
set(rewrite_expected "10.0.0.1 - - [21/Sep/2026:10:00:01 +0200] \"GET https://example.com/a%20b?x=1 HTTP/1.1\" 200 512
10.0.0.2 - - [21/Sep/2026:10:00:02 +0200] \"GET https://user:pass@example.com:8080/a/b HTTP/1.1\" 200 1024
10.0.0.3 - - [21/Sep/2026:10:00:03 +0200] \"GET https://example.com/path;p=1 HTTP/1.1\" 404 128
10.0.0.4 - - [21/Sep/2026:10:00:04 +0200] \"GET https://example.com/ü HTTP/1.1\" 200 256
10.0.0.5 - - [21/Sep/2026:10:00:05 +0200] \"POST https://example.com/submit HTTP/1.1\" 500 64
")
if("${rewrite_out}" STREQUAL "${rewrite_expected}")
   message(STATUS "ok   rewrite rewrites one line and leaves the rest")
else()
   message("FAIL rewrite rewrites one line and leaves the rest\n  got: [${rewrite_out}]")
   set_property(GLOBAL APPEND PROPERTY failures "rewrite output")
endif()
require_absent("rewrite prints no summary" "${rewrite_out}" "urlrewrite")

run(rewrite_ref --rewrite "(a%20b)" "<\\1>" "${FIXTURES}/access.log")
require_match("a backreference is expanded" "${rewrite_ref_out}" "<a%20b>")


# ----------------------------------------------------------------------- http

run(http_response --http "${FIXTURES}/response.txt")
require_exit("a complete response stream exits 0" "${http_response_rc}" "0")
require_match("both pipelined messages are parsed" "${http_response_out}"
   "httpparse: messages=2 headers=5 bytes=155 parsed=155 errno=HPE_OK")
require_match("the status line is reported" "${http_response_out}" "status +200 OK")
require_match("the framing is reported" "${http_response_out}"
   "framing +response HTTP/1.1 +content-length=11 +keep-alive=1")

run(http_request --http "${FIXTURES}/request.txt")
require_exit("a complete request exits 0" "${http_request_rc}" "0")
require_match("the request is parsed" "${http_request_out}"
   "httpparse: messages=1 headers=4 bytes=152 parsed=152 errno=HPE_OK")
require_match("the request target is reported" "${http_request_out}"
   "target +/submit.x=1")

run(http_bad --http "${FIXTURES}/bad-request.txt")
require_exit("a malformed request exits 1" "${http_bad_rc}" "1")
require_match("the header error is named" "${http_bad_out}"
   "HPE_INVALID_HEADER_TOKEN")

# The incremental parser cannot know the input ended until it is told, so a
# truncated message has to come out as an error and not as a short success.
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "GET /x HTTP/1.1"
                OUTPUT_FILE "${WORKDIR}/truncated.txt")
file(APPEND "${WORKDIR}/truncated.txt" "Host: example.com\nContent-Length: 5\n\nab")
run(http_truncated --http "${WORKDIR}/truncated.txt")
require_exit("a truncated message exits 1" "${http_truncated_rc}" "1")
require_match("a truncated message is not a success" "${http_truncated_out}"
   "HPE_INVALID_EOF_STATE")


# -------------------------------------------------------------- failure paths

run(no_pattern --extract)
require_exit("--extract without a pattern fails" "${no_pattern_rc}" "1")
require_match("--extract without a pattern explains" "${no_pattern_err}"
   "--extract needs a pattern")

run(no_replacement --rewrite "http://")
require_exit("--rewrite without a replacement fails" "${no_replacement_rc}" "1")
require_match("--rewrite without a replacement explains" "${no_replacement_err}"
   "--rewrite needs a replacement")

# \d is a compile error in this engine on purpose, and the message has to say
# which pattern it refused.
run(pcre_pattern --extract "\\d" "${FIXTURES}/access.log")
require_exit("a PCRE-only escape fails" "${pcre_pattern_rc}" "1")
require_match("a PCRE-only escape names the pattern" "${pcre_pattern_err}"
   "malformed pattern")

run(unknown --not-an-option)
require_exit("an unknown option fails" "${unknown_rc}" "1")
require_match("an unknown option explains" "${unknown_err}" "unknown option")

run(missing_file "${WORKDIR}/does-not-exist.txt")
require_exit("a missing file fails" "${missing_file_rc}" "1")

run(help --help)
require_exit("--help exits 0" "${help_rc}" "0")
# the demos print argv[0], so the name here is a path and, on Windows, carries
# an extension: [^ ]* absorbs both without weakening "names the program"
require_match("--help prints the usage" "${help_out}"
   "usage: .*urltoolkit[^ ]* .*options.*FILE")
require_match("--help lists the modes" "${help_out}" "--crosscheck")
require_match("--help lists --http" "${help_out}" "--http")


get_property(failed GLOBAL PROPERTY failures)
if(failed)
   list(LENGTH failed n)
   message(FATAL_ERROR "${n} urltoolkit test(s) failed: ${failed}")
endif()

message(STATUS "all urltoolkit tests passed")
