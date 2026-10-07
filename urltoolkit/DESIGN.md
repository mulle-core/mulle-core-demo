# urltoolkit design

The two splitters, the verdicts, and what the 35-URL corpus actually measured.
The short version is in [README.md](README.md).

## Modes

```
usage: urltoolkit [options] [FILE]

Reads one URL (or one line of text) per line from FILE, --stdin or
standard input, so the usual invocation is a pipe.

  --validate         split each URL and validate every component with
                     mulle-url (default)
  --fields           print the components http_parser_parse_url() finds
  --crosscheck       both, and classify the disagreements
  --slug             slugify each line, both flavors
  --extract PATTERN  print the part of each line that matches
  --rewrite P R      replace P with R in each line
  --http             parse the input as raw HTTP message(s)

  -q, --quiet        print only the summary (and any parse failure)
  -h, --help         this text
```

It reads one URL per line and never opens a socket: `mulle-core` has an HTTP
*parser* and no client and no TLS, so this is a linter and a rewriter.

## Design

Every URL is split twice — once by the demo's own splitter, whose components are
then put through `mulle-url`'s per-codepoint classifiers, and once by
`http_parser_parse_url()` — and `--crosscheck` classifies the differences instead
of picking a winner:

```
example.com:8080/path
  parser    FAIL     (read the pre-colon part as a scheme)
  ours      ok      host=example.com port=8080 path=/path
  verdict   disagree  host:port here, scheme there

http://example.com/a|b
  parser    ok       schema host path
  ours      INVALID path U+007C at codepoint 2   scheme=http host=example.com path=/a|b
  verdict   disagree  one of the two rejects it
```

Over the 35-URL fixture corpus: 25 agree, 7 differ on the verdict, 2 on
structure (a protocol-relative URL, and an empty `#` fragment — present to us,
absent to the parser) and 1 is the `host:port`-versus-scheme ambiguity above.

The port and the percent-escape are the two places where a per-codepoint
classifier is the wrong shape, and mulle-url answers both with a slice-level
check. The seven ports go through `mulle_unicode_is_validurlport` (`:abc` is
rejected on `a`), and the three `%`-bearing URLs through
`mulle_unicode_is_validurlpercentencoding` (`a%20b`, `%7euser`, the `%2F`
query). `:99999` is a syntactically valid port, so it is accepted and only
*noted* as outside 0..65535 (`range=1`), which keeps the demo's range reading
from being mistaken for mulle-url's verdict.

The three `;`-bearing URLs (`http://example.com/a;b`, `…/a;b?x=1` and
`…/path;p=1`) used to be the bulk of the verdict disagreements; `;` is a `pchar`
in RFC 3986 and mulle-url now accepts it, so they agree too (see `fixed/23`).
Two of the 35 never reach a component classifier at all: their non-ASCII is above
the BMP, and the `*plane` query the demo keeps in front of every classifier
rejects the codepoint in O(1) — the summary counts those as `pruned=2`.

**What it exercises.** `mulle-url` (all eight component classifiers, the UTF-32
forms, the plane queries as pruning, and the slice-level percent-escape check),
`mulle-http` (`http_parser_parse_url` as the reference split, and
`http_parser_execute` with its callback API in `--http`), `mulle-regex` (compile
once, `execute`, `range_for_index(0)` to slice out a match, `substitute` to
rewrite, and `\1` expansion), `mulle-slug` (both flavors, into allocated strings
and into a caller's buffer), plus `mulle-utf` for the UTF-8/UTF-32 conversions
all of it needs, `mulle-buffer`, `mulle-fprintf` and `mulle-allocator`.

## Limitations

No fetching, no normalizing, and no opinion about which side is right when the two
disagree: `example.com:8080/x` is a scheme to RFC 3986 and a host plus a port to
everyone else, and the demo prints both. The splitter is the demo's code —
`mulle-url` answers "is this codepoint legal in this component", it does not cut a
URL up. The corpus is a fixture, not a crawl, so the counts above are that file's
counts.
