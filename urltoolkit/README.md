# urltoolkit — URLs checked, cross-checked and rewritten

Every URL is split twice — once by the demo's own splitter, whose components are
then put through `mulle-url`'s per-codepoint classifiers, and once by
`http_parser_parse_url()` — and `--crosscheck` **classifies the disagreements**
instead of picking a winner. It reads one URL per line and never opens a socket:
`mulle-core` has an HTTP *parser* and no client and no TLS, so this is a linter and
a rewriter.

```sh
./build/urltoolkit/urltoolkit --validate urltoolkit/fixtures/urls.txt
./build/urltoolkit/urltoolkit --crosscheck --quiet < urltoolkit/fixtures/urls.txt
./build/urltoolkit/urltoolkit --slug < urltoolkit/fixtures/titles.txt
./build/urltoolkit/urltoolkit --extract 'https?://[[:alnum:].%/-]+' < access.log
./build/urltoolkit/urltoolkit --http urltoolkit/fixtures/response.txt
```

`-h` prints the modes. `--fields` prints what the reference parser found, `--rewrite
P R` does regex substitution, `-q` reduces everything to the summary.

**Numbers** — the 35-URL fixture corpus, via `--crosscheck`: **25 agree**, 7 differ on
the verdict, 2 on structure (a protocol-relative URL, and an empty `#` fragment —
present to us, absent to the parser) and 1 is the `host:port`-versus-scheme
ambiguity above. Separately, 2 of the 35 never reach a component classifier at all
— their non-ASCII is above the BMP and the `*plane` query in front of every
classifier rejects the codepoint in O(1) — which the summary counts as `pruned=2`.

**Strengths**

- **Two independent splitters, and the differences are the output.** The demo does
  not pretend the two agree; `--crosscheck` counts how and where they part, which is
  what makes the corpus a measurement rather than a fixture.
- **It found and closed a real gap.** The three `;`-bearing URLs used to be the bulk
  of the disagreements; `;` is a `pchar` in RFC 3986 and `mulle-url` now accepts it,
  so they agree (see `fixed/23`).
- **The port and the percent-escape are the two places a per-codepoint classifier is
  the wrong shape**, and `mulle-url` answers both with a slice-level check: the seven
  ports go through `mulle_unicode_is_validurlport` (`:abc` is rejected on `a`), the
  three `%`-bearing URLs through `mulle_unicode_is_validurlpercentencoding`
  (`a%20b`, `%7euser`, the `%2F` query).
- **The demo's own reading is kept apart from the library's verdict**: `:99999` is a
  syntactically valid port, so it is accepted and only *noted* as outside 0..65535
  (`range=1`), so its range check cannot be mistaken for `mulle-url` saying no.

**Limitations, on purpose**

- **No fetching, no TLS, no normalizing**, and **no opinion about who is right** when
  the two disagree: `example.com:8080/x` is a scheme to RFC 3986 and a host plus a
  port to everyone else, and the demo prints both rather than deciding.
- **The splitter is the demo's code.** `mulle-url` answers "is this codepoint legal
  in this component"; it does not cut a URL up, so the cross-check compares a demo
  splitter against `http_parser_parse_url()`, not `mulle-url` against it.
- **The corpus is a fixture, not a crawl** — 35 URLs — so every count here is that
  file's count and nothing more.

**What it exercises.** `mulle-url` (all eight component classifiers, the UTF-32
forms, the plane queries as pruning, and the slice-level percent-escape check),
`mulle-http` (`http_parser_parse_url` as the reference split, and
`http_parser_execute` with its callback API in `--http`), `mulle-regex` (compile
once, `execute`, `range_for_index(0)` to slice out a match, `substitute` to rewrite,
and `\1` expansion), `mulle-slug` (both flavors, into allocated strings and into a
caller's buffer), plus `mulle-utf` for the UTF-8/UTF-32 conversions all of it needs,
`mulle-buffer`, `mulle-fprintf` and `mulle-allocator`.

The corpus, the example disagreements and what each verdict means:
[DESIGN.md](DESIGN.md).
