/*
 * urltoolkit — URLs and text, checked and rewritten, built on mulle-core.
 *
 *   urltoolkit --validate fixtures/urls.txt
 *   urltoolkit --crosscheck --quiet < access.log
 *   urltoolkit --slug < fixtures/titles.txt
 *   urltoolkit --extract 'http[s]?://[[:alnum:].-]+' < access.log
 *   urltoolkit --http fixtures/response.txt
 *
 * Four constituents that nothing else in this repo used, on the critical path:
 *
 *   mulle-url     the component classifiers, one codepoint at a time, plus the
 *                 slice-level checks a codepoint function cannot express: the
 *                 percent-escape triplet and the port class. The demo asks a
 *                 classifier about every codepoint of every component, so "is
 *                 this a legal path character" *is* the program. The *plane
 *                 queries are used the way they are meant to be: a plane that
 *                 holds no valid character rejects the codepoint without a
 *                 component call, cached per slice and counted in the summary.
 *   mulle-http    nodejs's http_parser. http_parser_parse_url() is the
 *                 reference split that the demo's own splitter is checked
 *                 against, and --http drives the incremental parser over a raw
 *                 message through its callback API.
 *   mulle-regex   a UTF-32 regex with no PCRE ambitions (no \d, no \w, no
 *                 {n,m}, no lookaround). --extract uses execute() plus
 *                 range_for_index() to slice the match out of the line;
 *                 --rewrite uses mulle_utf32_substitute() and so runs compile,
 *                 match, substitute and convert end to end.
 *   mulle-slug    --slug, in both flavors: transliterate-and-drop versus keep
 *                 the unrepresentable letters, into an allocated string and
 *                 into a caller's buffer with a chosen delimiter.
 *
 * Plus mulle-utf for the UTF-8 <-> UTF-32 conversions all of this needs,
 * mulle-buffer for the input and for accumulating header chunks,
 * mulle-fprintf for output and mulle-allocator underneath everything.
 *
 * What it does not do is fetch. mulle-core has an HTTP *parser* and no client
 * and no TLS, so this reads bytes and never opens a socket: a linter and a
 * rewriter, not a crawler.
 *
 * The splitter is the demo's own code on purpose. mulle-url validates the
 * characters *inside* a component and does not cut a URL into components, and
 * neither library normalizes anything -- so where the two readings of a URL
 * differ, the demo reports the difference instead of picking a winner. Both are
 * usually defensible: `example.com:8080/x` is a host and a port to everyone
 * alive, and a perfectly good scheme to RFC 3986.
 */

#include <mulle-core/mulle-core.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define SLICE_TEXT_SIZE   256


/*
 * ------------------------------------------------------------------
 * options
 * ------------------------------------------------------------------
 */

enum mode
{
   mode_validate = 0,
   mode_fields,
   mode_crosscheck,
   mode_slug,
   mode_extract,
   mode_rewrite,
   mode_http
};

static struct
{
   enum mode    mode;
   int          quiet;
   int          use_stdin;
   const char  *pattern;
   const char  *replacement;
   const char  *path;
} options;


static const char   *mode_name( enum mode mode)
{
   switch( mode)
   {
   case mode_validate   : return( "validate");
   case mode_fields     : return( "fields");
   case mode_crosscheck : return( "crosscheck");
   case mode_slug       : return( "slug");
   case mode_extract    : return( "extract");
   case mode_rewrite    : return( "rewrite");
   case mode_http       : return( "http");
   }
   return( "?");
}


static void   usage( FILE *fp, const char *name)
{
   mulle_fprintf( fp,
      "usage: %s [options] [FILE]\n"
      "\n"
      "Reads one URL (or one line of text) per line from FILE, --stdin or\n"
      "standard input, so the usual invocation is a pipe.\n"
      "\n"
      "  --validate         split each URL and validate every component with\n"
      "                     mulle-url (default)\n"
      "  --fields           print the components http_parser_parse_url() finds\n"
      "  --crosscheck       both, and classify the disagreements\n"
      "  --slug             slugify each line, both flavors\n"
      "  --extract PATTERN  print the part of each line that matches\n"
      "  --rewrite P R      replace P with R in each line\n"
      "  --http             parse the input as raw HTTP message(s)\n"
      "\n"
      "  -q, --quiet        print only the summary (and any parse failure)\n"
      "  -h, --help         this text\n"
      "\n"
      "Examples:\n"
      "  %s --validate fixtures/urls.txt\n"
      "  cat access.log | %s --extract 'https?://[[:alnum:].%%/-]+'\n"
      "  %s --slug < fixtures/titles.txt\n"
      "  %s --http fixtures/response.txt\n",
      name, name, name, name, name);
}


static int   parse_arguments( int argc, char *argv[])
{
   int    i;
   char  *arg;

   options.mode = mode_validate;

   for( i = 1; i < argc; i++)
   {
      arg = argv[ i];

      if( ! strcmp( arg, "-h") || ! strcmp( arg, "--help"))
      {
         usage( stdout, argv[ 0]);
         exit( 0);
      }
      else if( ! strcmp( arg, "-q") || ! strcmp( arg, "--quiet"))
         options.quiet = 1;
      else if( ! strcmp( arg, "--stdin") || ! strcmp( arg, "-"))
         options.use_stdin = 1;
      else if( ! strcmp( arg, "--validate"))
         options.mode = mode_validate;
      else if( ! strcmp( arg, "--fields"))
         options.mode = mode_fields;
      else if( ! strcmp( arg, "--crosscheck"))
         options.mode = mode_crosscheck;
      else if( ! strcmp( arg, "--slug"))
         options.mode = mode_slug;
      else if( ! strcmp( arg, "--http"))
         options.mode = mode_http;
      else if( ! strcmp( arg, "--extract") || ! strcmp( arg, "--rewrite"))
      {
         options.mode = ! strcmp( arg, "--extract") ? mode_extract : mode_rewrite;
         if( i + 1 >= argc)
         {
            mulle_fprintf( stderr, "%s: %s needs a pattern\n", argv[ 0], arg);
            return( -1);
         }
         options.pattern = argv[ ++i];
         if( options.mode == mode_rewrite)
         {
            if( i + 1 >= argc)
            {
               mulle_fprintf( stderr, "%s: --rewrite needs a replacement\n", argv[ 0]);
               return( -1);
            }
            options.replacement = argv[ ++i];
         }
      }
      else if( arg[ 0] == '-' && arg[ 1])
      {
         mulle_fprintf( stderr, "%s: unknown option %s\n", argv[ 0], arg);
         usage( stderr, argv[ 0]);
         return( -1);
      }
      else if( options.path)
      {
         mulle_fprintf( stderr, "%s: one FILE at a time\n", argv[ 0]);
         return( -1);
      }
      else
         options.path = arg;
   }
   return( 0);
}


/*
 * ------------------------------------------------------------------
 * input, and one line at a time
 * ------------------------------------------------------------------
 */

static int   read_input( struct mulle_buffer *buffer, const char *name)
{
   char     scratch[ 4096];
   FILE    *fp;
   size_t   n;

   fp = options.use_stdin || ! name ? stdin : fopen( name, "rb");
   if( ! fp)
   {
      perror( name ? name : "stdin");
      return( -1);
   }

   while( ! feof( fp))
   {
      n = fread( scratch, 1, sizeof( scratch), fp);
      if( n)
         mulle_buffer_add_bytes( buffer, scratch, n);
      if( ferror( fp))
      {
         mulle_fprintf( stderr, "urltoolkit: %s: read error\n",
                        name ? name : "stdin");
         if( fp != stdin)
            fclose( fp);
         return( -1);
      }
   }

   if( fp != stdin)
      fclose( fp);
   return( 0);
}


typedef int   (*line_callback)( const char *line, void *context);

static void   each_line( struct mulle_buffer *buffer, line_callback callback,
                         void *context)
{
   const char   *bytes;
   const char   *end;
   const char   *line;

   /*
    * get_bytes(), not get_string(): get_string() *appends* a NUL and grows the
    * buffer's length by it, so a length taken afterwards is one too many and
    * every input would grow a phantom empty line.
    */
   bytes = mulle_buffer_get_bytes( buffer);
   if( ! bytes)
      return;

   end = bytes + mulle_buffer_get_length( buffer);
   for( line = bytes; line < end; )
   {
      const char   *nl;
      size_t        len;
      char         *copy;

      nl  = memchr( line, '\n', (size_t) (end - line));
      len = nl ? (size_t) (nl - line) : (size_t) (end - line);
      if( len && line[ len - 1] == '\r')
         len--;

      if( len)
      {
         copy = mulle_allocator_malloc( NULL, len + 1);
         memcpy( copy, line, len);
         copy[ len] = 0;
         callback( copy, context);
         mulle_free( copy);
      }

      line = nl ? nl + 1 : end;
   }
}


/*
 * ------------------------------------------------------------------
 * slicing
 *
 * [ scheme ":" ] [ "//" [userinfo "@"] host [":" port] ] path ["?" query]
 * ["#" fragment]. The demo splits, because mulle-url validates the characters
 * inside a component and does not cut one up.
 * ------------------------------------------------------------------
 */

struct slice
{
   const char   *s;
   size_t        len;
};

struct parts
{
   struct slice   scheme;
   struct slice   user;         /* userinfo before the first ':' */
   struct slice   password;     /* ... and after it, if any */
   struct slice   host;
   struct slice   port;
   struct slice   path;
   struct slice   query;
   struct slice   fragment;
   int            has_authority;
};


static void   slice_set( struct slice *slice, const char *s, const char *end)
{
   slice->s   = s;
   slice->len = s ? (size_t) (end - s) : 0;
}


static int   all_digits( const char *s, const char *end)
{
   if( s == end)
      return( 0);
   for( ; s < end; s++)
      if( *s < '0' || *s > '9')
         return( 0);
   return( 1);
}


static void   split_url( const char *url, struct parts *parts)
{
   const char   *p;
   const char   *end;
   const char   *colon;
   int           brackets;

   memset( parts, 0, sizeof( *parts));
   end = url + strlen( url);

   p = strchr( url, '#');
   if( p)
   {
      slice_set( &parts->fragment, p + 1, end);
      end = p;
   }

   p = memchr( url, '?', (size_t) (end - url));
   if( p)
   {
      slice_set( &parts->query, p + 1, end);
      end = p;
   }

   /*
    * The scheme colon is the first ':' outside a host's IPv6 brackets. A purely
    * numeric tail after it is read as a port instead, because `example.com:8080/x`
    * is a scheme to RFC 3986 and a host and a port to everyone else;
    * --crosscheck names that ambiguity rather than hiding it.
    */
   colon    = NULL;
   brackets = 0;
   for( p = url; p < end; p++)
   {
      if( *p == '[')
         brackets++;
      else
         if( *p == ']')
            brackets--;
         else
            if( *p == ':' && ! brackets)
            {
               colon = p;
               break;
            }
   }

   if( colon && colon > url)
   {
      const char   *stop;

      stop = memchr( colon + 1, '/', (size_t) (end - colon - 1));
      if( ! stop)
         stop = end;

      if( ! all_digits( colon + 1, stop))
      {
         slice_set( &parts->scheme, url, colon);
         url = colon + 1;
      }
      else
         if( ! (url[ 0] == '/' && url[ 1] == '/'))
         {
            /*
             * `host:port/path` with no scheme and no `//`: the pre-colon part
             * is a host and the tail is a port. RFC 3986 reads this as a scheme
             * and then a relative path, which is what http_parser_parse_url()
             * does -- --crosscheck reports the two readings side by side.
             */
            slice_set( &parts->host, url, colon);
            slice_set( &parts->port, colon + 1, stop);
            url = stop;
         }
   }

   if( url + 1 < end && url[ 0] == '/' && url[ 1] == '/')
   {
      struct slice   userinfo;
      const char    *authority;
      const char    *authority_end;
      const char    *stop;

      parts->has_authority = 1;
      authority            = url + 2;
      authority_end        = authority;
      while( authority_end < end && *authority_end != '/')
         authority_end++;
      stop                 = authority_end;

      memset( &userinfo, 0, sizeof( userinfo));
      p = memchr( authority, '@', (size_t) (stop - authority));
      if( p)
      {
         slice_set( &userinfo, authority, p);
         authority = p + 1;
      }

      /*
       * userinfo is `user[:password]`: http_parser calls the whole thing one
       * field, mulle-url has a character class for each half.
       */
      if( userinfo.s)
      {
         const char   *split;

         split = memchr( userinfo.s, ':', userinfo.len);
         if( split)
         {
            slice_set( &parts->user, userinfo.s, split);
            slice_set( &parts->password, split + 1, userinfo.s + userinfo.len);
         }
         else
            slice_set( &parts->user, userinfo.s, userinfo.s + userinfo.len);
      }

      p = NULL;
      if( authority < stop)
      {
         if( *authority == '[')
         {
            const char   *close;

            close = memchr( authority, ']', (size_t) (stop - authority));
            if( close)
               p = memchr( close, ':', (size_t) (stop - close));
         }
         else
         {
            const char   *scan;

            for( scan = authority; scan < stop; scan++)
               if( *scan == ':')
                  p = scan;
         }
      }
      if( p)
      {
         slice_set( &parts->port, p + 1, stop);
         stop = p;
      }
      slice_set( &parts->host, authority, stop);
      url = authority_end;      /* not `stop`, which the port moved */
   }

   if( url < end)
      slice_set( &parts->path, url, end);
}


static void   print_slice( FILE *fp, struct slice slice)
{
   char     text[ SLICE_TEXT_SIZE];
   size_t   len;

   if( ! slice.s)
      return;

   len = slice.len;
   if( len >= sizeof( text))
      len = sizeof( text) - 1;
   memcpy( text, slice.s, len);
   text[ len] = 0;
   mulle_fprintf( fp, "%s", text);
   if( slice.len > len)
      mulle_fprintf( fp, "...");
}


/*
 * ------------------------------------------------------------------
 * validation
 *
 * One classifier call per codepoint, with the plane query in front of it: a
 * plane holding no valid character rejects the codepoint without asking the
 * component. The plane is cached because a slice is almost always one plane,
 * and both are counted so the summary can show the work that was done.
 * ------------------------------------------------------------------
 */

struct checks
{
   unsigned long long   codepoints;
   unsigned long long   plane_pruned;
   unsigned long long   invalid_components;
   unsigned long long   undecodable;
};


static int   slice_is_valid( struct slice slice, int (*is_valid)( int32_t),
                             int (*is_validplane)( unsigned int),
                             int allows_percent,
                             int32_t *bad_codepoint, size_t *bad_index,
                             struct checks *checks)
{
   mulle_utf32_t   *utf32;
   unsigned int     plane;
   int              plane_verdict;
   size_t           i;

   *bad_codepoint = 0;
   *bad_index     = 0;

   if( ! slice.s || ! slice.len)
      return( 1);

   utf32 = mulle_utf8_convert_to_utf32_string( slice.s, slice.len, NULL);
   if( ! utf32)
   {
      checks->undecodable++;
      return( 0);
   }

   /*
    * A '%' is a codepoint every component classifier rejects: the escape is a
    * *triplet*, which no per-codepoint function can see. mulle-url ships the
    * slice-level check for exactly that, so the demo asks it first and then
    * steps over every well-formed triplet in the loop below -- the two hex
    * digits are ASCII and the component allows pct-encoded by construction.
    */
   if( allows_percent && ! mulle_unicode_is_validurlpercentencoding( slice.s, slice.len))
   {
      for( i = 0; utf32[ i]; i++)
         if( utf32[ i] == '%')
            break;

      *bad_codepoint = '%';
      *bad_index     = i;
      mulle_free( utf32);
      return( 0);
   }

   plane         = 0;
   plane_verdict = 1;
   for( i = 0; utf32[ i]; i++)
   {
      checks->codepoints++;

      if( (unsigned int) (utf32[ i] >> 16) != plane)
      {
         plane         = (unsigned int) (utf32[ i] >> 16);
         plane_verdict = (*is_validplane)( plane);
      }

      if( ! plane_verdict)
      {
         checks->plane_pruned++;
         *bad_codepoint = utf32[ i];
         *bad_index     = i;
         mulle_free( utf32);
         return( 0);
      }

      if( utf32[ i] == '%' && allows_percent)
      {
         i += 2;
         continue;
      }

      if( ! (*is_valid)( utf32[ i]))
      {
         *bad_codepoint = utf32[ i];
         *bad_index     = i;
         mulle_free( utf32);
         return( 0);
      }
   }

   mulle_free( utf32);
   return( 1);
}


/*
 * mulle-url validates the port's *characters* (its classifier is RFC 3986
 * port = *DIGIT), but a port is also a number, and one above 65535 is one
 * nobody can connect to. That reading is the demo's, it does not change the
 * verdict, and the summary counts it on its own line so the two are never
 * confused.
 */
static long   port_number( struct slice port)
{
   unsigned long long   n;
   size_t               i;

   n = 0;
   for( i = 0; i < port.len; i++)
      if( n <= 100000000ULL)
         n = n * 10 + (unsigned long long) (port.s[ i] - '0');
   return( (long) n);
}


struct validation
{
   int            ok;
   const char    *component;
   int32_t        codepoint;
   size_t         index;
   int            port_seen;          /* a port was present */
   long           port;               /* its numeric reading */
   int            port_out_of_range;  /* the demo's interpretation, not the verdict */
};


static void   validate_parts( struct parts *parts, struct validation *validation,
                              struct checks *checks)
{
   struct component
   {
      const char   *name;
      struct slice  slice;
      int         (*is_valid)( int32_t);
      int         (*is_validplane)( unsigned int);
      int           allows_percent;   /* pct-encoded = "%" HEXDIG HEXDIG */
   };
   struct component   components[] =
   {
      { "scheme",   parts->scheme,   mulle_unicode_is_validurlscheme,
        mulle_unicode_is_validurlschemeplane, 0 },
      { "host",     parts->host,     mulle_unicode_is_validurlhost,
        mulle_unicode_is_validurlhostplane, 1 },
      { "port",     parts->port,     mulle_unicode_is_validurlport,
        mulle_unicode_is_validurlportplane, 0 },
      { "user",     parts->user,     mulle_unicode_is_validurluser,
        mulle_unicode_is_validurluserplane, 1 },
      { "password", parts->password, mulle_unicode_is_validurlpassword,
        mulle_unicode_is_validurlpasswordplane, 1 },
      { "path",     parts->path,     mulle_unicode_is_validurlpath,
        mulle_unicode_is_validurlpathplane, 1 },
      { "query",    parts->query,    mulle_unicode_is_validurlquery,
        mulle_unicode_is_validurlqueryplane, 1 },
      { "fragment", parts->fragment, mulle_unicode_is_validurlfragment,
        mulle_unicode_is_validurlfragmentplane, 1 }
   };
   size_t   i;

   memset( validation, 0, sizeof( *validation));

   for( i = 0; i < sizeof( components) / sizeof( components[ 0]); i++)
      if( ! slice_is_valid( components[ i].slice, components[ i].is_valid,
                            components[ i].is_validplane,
                            components[ i].allows_percent,
                            &validation->codepoint, &validation->index, checks))
      {
         checks->invalid_components++;
         validation->component = components[ i].name;
         return;
      }

   /*
    * The port is part of the demo's report even though mulle-url only judges
    * its characters: a port that does not fit in 16 bits is a fact the caller
    * asked for. It is a note on a valid URL, not a verdict of its own.
    */
   if( parts->port.s && parts->port.len)
   {
      validation->port_seen         = 1;
      validation->port              = port_number( parts->port);
      validation->port_out_of_range = validation->port > 65535;
   }
   validation->ok = 1;
}


/*
 * ------------------------------------------------------------------
 * http_parser_parse_url: the reference split
 * ------------------------------------------------------------------
 */

static const char   *field_name( int field)
{
   static const char   *names[] = { "schema", "host", "port", "path", "query",
                                    "fragment", "userinfo" };

   return( field >= 0 && field < UF_MAX ? names[ field] : "?");
}


struct parser_split
{
   int   rval;
   int   present[ UF_MAX];
   int   port;
};


static void   parse_url_with_parser( const char *url, struct parser_split *split)
{
   struct http_parser_url   u;
   int                      i;

   memset( split, 0, sizeof( *split));
   http_parser_url_init( &u);
   split->rval = http_parser_parse_url( url, strlen( url), 0, &u);

   /*
    * field_set is filled in even when the parse fails -- on `example.com:8080/x`
    * it says "schema", which is how the two readings become visible at all.
    * Only the presence of the fields is taken from a failed parse; the fields
    * themselves are not compared.
    */
   for( i = 0; i < UF_MAX; i++)
      if( u.field_set & (1 << i))
         split->present[ i] = 1;
   split->port = u.port;
}


static void   print_parser_fields( FILE *fp, const struct parser_split *split)
{
   int   i;

   for( i = 0; i < UF_MAX; i++)
      if( split->present[ i])
      {
         if( i == UF_PORT)
            mulle_fprintf( fp, " %s=%d", field_name( i), split->port);
         else
            mulle_fprintf( fp, " %s", field_name( i));
      }
}


static void   print_our_fields( FILE *fp, const struct parts *parts)
{
   static const char   *labels[] = { "scheme", "user", "password", "host",
                                     "port", "path", "query", "fragment" };
   struct slice         slices[] = { parts->scheme, parts->user, parts->password,
                                     parts->host, parts->port, parts->path,
                                     parts->query, parts->fragment };
   size_t               i;

   for( i = 0; i < sizeof( slices) / sizeof( slices[ 0]); i++)
   {
      if( ! slices[ i].s)
         continue;
      mulle_fprintf( fp, " %s=", labels[ i]);
      print_slice( fp, slices[ i]);
   }
}


/*
 * ------------------------------------------------------------------
 * the two reporting modes
 * ------------------------------------------------------------------
 */

enum category
{
   category_agree = 0,
   category_verdict,
   category_structure,
   category_ambiguity
};


struct counters
{
   unsigned long long   lines;
   unsigned long long   accepted;
   unsigned long long   parser_accepted;
   unsigned long long   agree;
   unsigned long long   verdict;
   unsigned long long   structure;
   unsigned long long   ambiguity;
   unsigned long long   ports;
   unsigned long long   ports_out_of_range;
   struct checks        checks;
};


static enum category   classify( struct parts *parts,
                                 const struct parser_split *split,
                                 int ours_ok)
{
   int   structure;

   /*
    * `example.com:8080/x`: the parser takes the pre-colon part for a scheme,
    * the demo takes it for a host and a port.
    */
   if( ! parts->scheme.s && parts->port.s && split->present[ UF_SCHEMA])
      return( category_ambiguity);

   if( split->rval)
      return( ours_ok ? category_verdict : category_agree);

   if( ! ours_ok)
      return( category_verdict);

   structure = 0;
   if( (parts->scheme.s != NULL) != split->present[ UF_SCHEMA])
      structure = 1;
   if( (parts->host.s != NULL) != split->present[ UF_HOST])
      structure = 1;
   if( (parts->port.s != NULL) != split->present[ UF_PORT])
      structure = 1;
   if( (parts->path.s != NULL) != split->present[ UF_PATH])
      structure = 1;
   if( (parts->query.s != NULL) != split->present[ UF_QUERY])
      structure = 1;
   if( (parts->fragment.s != NULL) != split->present[ UF_FRAGMENT])
      structure = 1;
   if( (parts->user.s != NULL) != split->present[ UF_USERINFO])
      structure = 1;

   return( structure ? category_structure : category_agree);
}


static int   report_url( const char *url, void *context)
{
   struct counters       *counters;
   struct parts           parts;
   struct parser_split    split;
   struct validation      validation;
   struct checks          checks;
   enum category          category;
   char                   text[ SLICE_TEXT_SIZE];
   size_t                 len;

   counters = context;
   counters->lines++;

   memset( &checks, 0, sizeof( checks));
   split_url( url, &parts);
   validate_parts( &parts, &validation, &checks);

   counters->checks.codepoints         += checks.codepoints;
   counters->checks.plane_pruned       += checks.plane_pruned;
   counters->checks.undecodable        += checks.undecodable;
   counters->checks.invalid_components += checks.invalid_components;
   if( validation.ok)
      counters->accepted++;
   if( parts.port.s)
      counters->ports++;
   if( validation.port_out_of_range)
      counters->ports_out_of_range++;

   len = strlen( url);
   if( len >= sizeof( text))
      len = sizeof( text) - 1;
   memcpy( text, url, len);
   text[ len] = 0;

   if( options.mode == mode_validate)
   {
      if( options.quiet)
         return( 0);

      if( validation.ok)
      {
         mulle_fprintf( stdout, "%-42s ok", text);
         print_our_fields( stdout, &parts);
         if( validation.port_out_of_range)
            mulle_fprintf( stdout, " (port %ld outside 0..65535)", validation.port);
         mulle_fprintf( stdout, "\n");
      }
      else
         mulle_fprintf( stdout, "%-42s INVALID  %s U+%04X at codepoint %lu\n",
                        text, validation.component ? validation.component : "?",
                        (unsigned int) validation.codepoint,
                        (unsigned long) validation.index);
      return( 0);
   }

   if( options.mode == mode_fields)
   {
      if( options.quiet)
         return( 0);
      parse_url_with_parser( url, &split);
      if( split.rval)
         mulle_fprintf( stdout, "%-42s parser FAIL\n", text);
      else
      {
         mulle_fprintf( stdout, "%-42s parser", text);
         print_parser_fields( stdout, &split);
         mulle_fprintf( stdout, "\n");
      }
      return( 0);
   }

   /* crosscheck */
   parse_url_with_parser( url, &split);
   if( ! split.rval)
      counters->parser_accepted++;

   category = classify( &parts, &split, validation.ok);
   switch( category)
   {
   case category_agree     : counters->agree++;      break;
   case category_verdict   : counters->verdict++;    break;
   case category_structure : counters->structure++;  break;
   case category_ambiguity : counters->ambiguity++;  break;
   }

   if( options.quiet)
      return( 0);

   mulle_fprintf( stdout, "%s\n", text);

   if( split.rval)
   {
      mulle_fprintf( stdout, "  %-9s FAIL", "parser");
      if( split.present[ UF_SCHEMA])
         mulle_fprintf( stdout, "     (read the pre-colon part as a scheme)");
      mulle_fprintf( stdout, "\n");
   }
   else
   {
      mulle_fprintf( stdout, "  %-9s ok      ", "parser");
      print_parser_fields( stdout, &split);
      mulle_fprintf( stdout, "\n");
   }

   if( validation.ok)
      mulle_fprintf( stdout, "  %-9s ok     ", "ours");
   else
      mulle_fprintf( stdout, "  %-9s INVALID %s U+%04X at codepoint %lu  ",
                     "ours", validation.component ? validation.component : "?",
                     (unsigned int) validation.codepoint,
                     (unsigned long) validation.index);
   print_our_fields( stdout, &parts);
   if( validation.port_out_of_range)
      mulle_fprintf( stdout, " (port %ld outside 0..65535)", validation.port);
   mulle_fprintf( stdout, "\n");

   switch( category)
   {
   case category_agree     :
      mulle_fprintf( stdout, "  %-9s agree\n\n", "verdict");
      break;
   case category_verdict   :
      mulle_fprintf( stdout, "  %-9s disagree  one of the two rejects it\n\n",
                     "verdict");
      break;
   case category_structure :
      mulle_fprintf( stdout, "  %-9s disagree  same verdict, different components\n\n",
                     "verdict");
      break;
   case category_ambiguity :
      mulle_fprintf( stdout, "  %-9s disagree  host:port here, scheme there\n\n",
                     "verdict");
      break;
   }
   return( 0);
}


/*
 * ------------------------------------------------------------------
 * slug
 * ------------------------------------------------------------------
 */

static int   slug_line( const char *line, void *context)
{
   char            buffer[ 512];
   char           *dropped;
   char           *kept;
   struct counters *counters;

   counters = context;
   counters->lines++;

   dropped = mulle_utf8_slugify( line);
   kept    = mulle_utf8_slugify_utf8( line);

   mulle_fprintf( stdout, "%-30s -> %-30s  (utf8 kept: %s)\n",
                  line,
                  dropped ? dropped : "(null)",
                  kept ? kept : "(null)");

   if( mulle_slugify( buffer, sizeof( buffer), line, (size_t) -1))
      mulle_fprintf( stdout, "%-30s    buffer, '-': %s\n", "", buffer);
   if( mulle_slugify_with_delimiter( buffer, sizeof( buffer), line,
                                     (size_t) -1, '_'))
      mulle_fprintf( stdout, "%-30s    buffer, '_': %s\n", "", buffer);

   mulle_free( dropped);
   mulle_free( kept);
   return( 0);
}


/*
 * ------------------------------------------------------------------
 * regex
 *
 * mulle-regex is UTF-32 only, so a line goes through mulle-utf on the way in
 * and again on the way out.
 * ------------------------------------------------------------------
 */

struct regex_context
{
   struct mulle_utf32regex   *regex;
   mulle_utf32_t             *pattern;
   mulle_utf32_t             *replacement;
   struct counters           *counters;
};


static int   extract_line( const char *line, void *context)
{
   struct regex_context   *regex_context;
   mulle_utf32_t          *utf32;
   mulle_utf32_t          *matched;
   struct mulle_range      range;
   char                   *utf8;
   size_t                  length;

   regex_context = context;
   regex_context->counters->lines++;

   utf32 = mulle_utf8_convert_to_utf32_string( line, (size_t) -1, NULL);
   if( ! utf32)
      return( 0);

   if( mulle_utf32regex_execute( regex_context->regex, utf32) == 1)
   {
      range   = mulle_utf32regex_range_for_index( regex_context->regex, 0);
      length  = range.length;
      matched = mulle_allocator_malloc( NULL, (length + 1) * sizeof( mulle_utf32_t));
      if( matched)
      {
         memcpy( matched, utf32 + range.location, length * sizeof( mulle_utf32_t));
         matched[ length] = 0;
         utf8 = mulle_utf32_convert_to_utf8_string( matched, length, NULL);
         if( utf8)
         {
            mulle_fprintf( stdout, "%s\n", utf8);
            mulle_free( utf8);
         }
         mulle_free( matched);
      }
   }

   mulle_free( utf32);
   return( 0);
}


static size_t   utf32_length( const mulle_utf32_t *s)
{
   size_t   length;

   for( length = 0; s[ length]; length++)
      ;
   return( length);
}


/*
 * A sed-style substitution, assembled by hand, because the library substitutes
 * the *match* and nothing else: "Substitution does not include prefix/suffix of
 * original string -- caller must stitch prefix + replacement + suffix". The
 * convenience call mulle_utf32_substitute() returns exactly that fragment, so a
 * caller who wants the line back has to do this.
 */
static int   rewrite_line( const char *line, void *context)
{
   struct regex_context   *regex_context;
   mulle_utf32_t          *utf32;
   mulle_utf32_t          *substituted;
   mulle_utf32_t          *result;
   struct mulle_range      range;
   size_t                  length;
   size_t                  substituted_length;
   size_t                  substituted_size;
   size_t                  suffix;
   char                   *utf8;

   regex_context = context;
   regex_context->counters->lines++;

   utf32 = mulle_utf8_convert_to_utf32_string( line, (size_t) -1, NULL);
   if( ! utf32)
      return( 0);

   if( mulle_utf32regex_execute( regex_context->regex, utf32) != 1)
   {
      mulle_fprintf( stdout, "%s\n", line);      /* no match: unchanged */
      mulle_free( utf32);
      return( 0);
   }

   range            = mulle_utf32regex_range_for_index( regex_context->regex, 0);
   substituted_size = mulle_utf32regex_substitution_buffer_size( regex_context->regex,
                                                                 regex_context->replacement);
   substituted      = mulle_allocator_malloc( NULL, substituted_size);
   if( ! substituted)
   {
      mulle_free( utf32);
      return( 0);
   }

   /* last argument 1: append the trailing zero, which the sizing accounted for */
   if( mulle_utf32regex_substitute( regex_context->regex,
                                    regex_context->replacement,
                                    substituted, substituted_size, 1) < 0)
   {
      mulle_free( substituted);
      mulle_free( utf32);
      return( 0);
   }

   length             = utf32_length( utf32);
   substituted_length = utf32_length( substituted);
   suffix             = length - range.location - range.length;

   result = mulle_allocator_malloc( NULL,
                                    (range.location + substituted_length + suffix + 1) *
                                    sizeof( mulle_utf32_t));
   if( result)
   {
      memcpy( result, utf32, range.location * sizeof( mulle_utf32_t));
      memcpy( result + range.location, substituted,
              substituted_length * sizeof( mulle_utf32_t));
      memcpy( result + range.location + substituted_length,
              utf32 + range.location + range.length,
              suffix * sizeof( mulle_utf32_t));
      result[ range.location + substituted_length + suffix] = 0;

      utf8 = mulle_utf32_convert_to_utf8_string( result, (size_t) -1, NULL);
      if( utf8)
      {
         mulle_fprintf( stdout, "%s\n", utf8);
         mulle_free( utf8);
      }
      mulle_free( result);
   }

   mulle_free( substituted);
   mulle_free( utf32);
   return( 0);
}


/*
 * ------------------------------------------------------------------
 * --http: the incremental parser, driven by its callbacks
 *
 * The one API here that hands you bytes in pieces instead of a finished
 * answer, which is why a header is printed when the *next* field starts: the
 * parser does not promise a header arrives whole or in one callback.
 * ------------------------------------------------------------------
 */

struct http_state
{
   struct mulle_buffer   field;
   struct mulle_buffer   value;
   struct mulle_buffer   url;
   int                   have_field;
   unsigned long long    headers;         /* of the current message */
   unsigned long long    headers_total;   /* of the whole stream */
   unsigned long long    body;
   unsigned long long    messages;
};


static int   on_message_begin( http_parser *parser)
{
   struct http_state   *state;

   state = parser->data;
   state->headers    = 0;
   state->body       = 0;
   state->have_field = 0;
   mulle_buffer_remove_all( &state->url);
   mulle_buffer_remove_all( &state->field);
   mulle_buffer_remove_all( &state->value);
   return( 0);
}


static int   on_url( http_parser *parser, const char *at, size_t length)
{
   struct http_state   *state;

   state = parser->data;
   mulle_buffer_add_bytes( &state->url, at, length);
   return( 0);
}


static int   on_status( http_parser *parser, const char *at, size_t length)
{
   struct http_state   *state;

   state = parser->data;
   mulle_buffer_add_bytes( &state->url, at, length);   /* reuse: reason phrase */
   return( 0);
}


static void   flush_header( struct http_state *state)
{
   if( ! state->have_field)
      return;

   if( ! options.quiet)
      mulle_fprintf( stdout, "  header   %s: %s\n",
                     mulle_buffer_get_string( &state->field),
                     mulle_buffer_get_string( &state->value));
   mulle_buffer_remove_all( &state->field);
   mulle_buffer_remove_all( &state->value);
   state->have_field = 0;
}


static int   on_header_field( http_parser *parser, const char *at, size_t length)
{
   struct http_state   *state;

   state = parser->data;
   flush_header( state);
   state->have_field = 1;
   mulle_buffer_add_bytes( &state->field, at, length);
   return( 0);
}


static int   on_header_value( http_parser *parser, const char *at, size_t length)
{
   struct http_state   *state;

   state = parser->data;
   state->headers++;
   state->headers_total++;
   mulle_buffer_add_bytes( &state->value, at, length);
   return( 0);
}


static int   on_headers_complete( http_parser *parser)
{
   struct http_state   *state;

   state = parser->data;
   flush_header( state);

   if( ! options.quiet)
      mulle_fprintf( stdout, "  framing  %s HTTP/%u.%u  content-length=%llu  keep-alive=%d\n",
                     parser->type == HTTP_REQUEST
                        ? http_method_str( (enum http_method) parser->method)
                        : "response",
                     parser->http_major, parser->http_minor,
                     (unsigned long long) parser->content_length,
                     http_should_keep_alive( parser));
   return( 0);
}


static int   on_body( http_parser *parser, const char *at, size_t length)
{
   struct http_state   *state;

   MULLE_C_UNUSED( at);          /* the body is only counted, not stored */
   state = parser->data;
   state->body += length;
   return( 0);
}


static int   on_message_complete( http_parser *parser)
{
   struct http_state   *state;

   state = parser->data;
   state->messages++;
   if( ! options.quiet)
   {
      if( parser->type == HTTP_RESPONSE)
         mulle_fprintf( stdout, "  status   %u %s\n", parser->status_code,
                        mulle_buffer_get_string( &state->url));
      else
         mulle_fprintf( stdout, "  target   %s\n",
                        mulle_buffer_get_string( &state->url));
      mulle_fprintf( stdout, "  body     %llu bytes, %llu header value(s)\n\n",
                     state->body, state->headers);
   }
   return( 0);
}


static int   run_http( struct mulle_buffer *buffer)
{
   struct http_parser_settings   settings;
   struct http_parser            parser;
   struct http_state             state;
   const char                   *bytes;
   size_t                        length;
   size_t                        parsed;
   enum http_errno               error;
   int                           rval;

   memset( &state, 0, sizeof( state));
   mulle_buffer_init( &state.field, 0, NULL);
   mulle_buffer_init( &state.value, 0, NULL);
   mulle_buffer_init( &state.url, 0, NULL);

   bytes  = mulle_buffer_get_bytes( buffer);
   length = mulle_buffer_get_length( buffer);

   http_parser_init( &parser, HTTP_BOTH);
   http_parser_settings_init( &settings);
   parser.data                  = &state;
   settings.on_message_begin    = on_message_begin;
   settings.on_url              = on_url;
   settings.on_status           = on_status;
   settings.on_header_field     = on_header_field;
   settings.on_header_value     = on_header_value;
   settings.on_headers_complete = on_headers_complete;
   settings.on_body             = on_body;
   settings.on_message_complete = on_message_complete;

   parsed = http_parser_execute( &parser, &settings, bytes, length);

   /*
    * Tell the parser the stream ended. With no more bytes it can decide
    * whether the state it is in may end there -- a message waiting for the rest
    * of its body is HPE_INVALID_EOF_STATE, not a success. Without this call a
    * truncated message is indistinguishable from a complete one.
    */
   if( HTTP_PARSER_ERRNO( &parser) == HPE_OK && parsed == length)
      (void) http_parser_execute( &parser, &settings, NULL, 0);

   error  = HTTP_PARSER_ERRNO( &parser);
   rval   = error == HPE_OK && parsed == length ? 0 : 1;

   if( error != HPE_OK)
      mulle_fprintf( stdout, "  %-8s %s: %s\n", "error", http_errno_name( error),
                     http_errno_description( error));
   else
      if( parsed != length)
         mulle_fprintf( stdout, "  %-8s stopped after %zu of %zu bytes,"
                                " with no error set\n",
                        "error", parsed, length);

   mulle_fprintf( stdout, "httpparse: messages=%llu headers=%llu bytes=%zu parsed=%zu errno=%s\n",
                  state.messages, state.headers_total, length, parsed,
                  http_errno_name( error));

   mulle_buffer_done( &state.field);
   mulle_buffer_done( &state.value);
   mulle_buffer_done( &state.url);
   return( rval);
}


/*
 * ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------
 */

static void   print_summary( struct counters *counters)
{
   mulle_fprintf( stdout, "\n=== Summary (%s) ===\n", mode_name( options.mode));
   mulle_fprintf( stdout, "Lines processed   : %llu\n", counters->lines);

   switch( options.mode)
   {
   case mode_validate :
      mulle_fprintf( stdout, "Valid URLs        : %llu\n", counters->accepted);
      mulle_fprintf( stdout, "Invalid URLs      : %llu\n",
                     counters->lines - counters->accepted);
      mulle_fprintf( stdout, "Invalid components: %llu\n",
                     counters->checks.invalid_components);
      mulle_fprintf( stdout, "Codepoints asked  : %llu\n",
                     counters->checks.codepoints);
      mulle_fprintf( stdout, "Plane-pruned      : %llu (rejected with no component call)\n",
                     counters->checks.plane_pruned);
      if( counters->checks.undecodable)
         mulle_fprintf( stdout, "Not UTF-8         : %llu\n",
                        counters->checks.undecodable);
      mulle_fprintf( stdout, "Ports seen        : %llu (mulle-url validates the digits;\n"
                             "                    the demo interprets the range)\n",
                     counters->ports);
      mulle_fprintf( stdout, "Ports out of range: %llu (a note on a valid URL, not a verdict)\n",
                     counters->ports_out_of_range);
      mulle_fprintf( stdout, "urlvalidate: urls=%llu ok=%llu invalid=%llu codepoints=%llu"
                             " pruned=%llu ports=%llu range=%llu\n",
                     counters->lines, counters->accepted,
                     counters->lines - counters->accepted,
                     counters->checks.codepoints, counters->checks.plane_pruned,
                     counters->ports, counters->ports_out_of_range);
      break;

   case mode_fields :
      mulle_fprintf( stdout, "urlfields: urls=%llu\n", counters->lines);
      break;

   case mode_crosscheck :
      mulle_fprintf( stdout, "Parser accepted   : %llu\n", counters->parser_accepted);
      mulle_fprintf( stdout, "Ours accepted     : %llu\n", counters->accepted);
      mulle_fprintf( stdout, "Agree             : %llu\n", counters->agree);
      mulle_fprintf( stdout, "Disagree          : %llu\n",
                     counters->verdict + counters->structure + counters->ambiguity);
      mulle_fprintf( stdout, "  verdict         : %llu (one of the two rejects it)\n",
                     counters->verdict);
      mulle_fprintf( stdout, "  structure       : %llu (same verdict, different components)\n",
                     counters->structure);
      mulle_fprintf( stdout, "  ambiguity       : %llu (host:port here, scheme there)\n",
                     counters->ambiguity);
      mulle_fprintf( stdout, "urlcheck: urls=%llu parser_ok=%llu ours_ok=%llu agree=%llu"
                             " verdict=%llu structure=%llu ambiguity=%llu ports=%llu\n",
                     counters->lines, counters->parser_accepted, counters->accepted,
                     counters->agree, counters->verdict, counters->structure,
                     counters->ambiguity, counters->ports);
      break;

   case mode_slug :
      mulle_fprintf( stdout, "urlslug: lines=%llu\n", counters->lines);
      break;

   case mode_extract :
      mulle_fprintf( stdout, "urlextract: lines=%llu\n", counters->lines);
      break;

   case mode_rewrite :
      mulle_fprintf( stdout, "urlrewrite: lines=%llu\n", counters->lines);
      break;

   case mode_http :
      break;
   }
}


int   main( int argc, char *argv[])
{
   struct mulle_buffer       buffer;
   struct counters           counters;
   struct mulle_utf32regex  *regex;
   struct regex_context      regex_context;
   mulle_utf32_t            *pattern;
   mulle_utf32_t            *replacement;
   int                       rval;

   if( parse_arguments( argc, argv))
      return( 1);

   memset( &counters, 0, sizeof( counters));

   if( options.mode == mode_extract || options.mode == mode_rewrite)
   {
      pattern = mulle_utf8_convert_to_utf32_string( options.pattern,
                                                    (size_t) -1, NULL);
      if( ! pattern)
      {
         mulle_fprintf( stderr, "%s: pattern is not valid UTF-8\n", argv[ 0]);
         return( 1);
      }

      regex = mulle_utf32regex_compile_options( pattern, MULLE_UTF32REGEX_EXTENDED);
      if( ! regex)
      {
         mulle_fprintf( stderr, "%s: malformed pattern `%s'\n", argv[ 0],
                        options.pattern);
         mulle_free( pattern);
         return( 1);
      }

      replacement = NULL;
      if( options.mode == mode_rewrite)
      {
         replacement = mulle_utf8_convert_to_utf32_string( options.replacement,
                                                           (size_t) -1, NULL);
         if( ! replacement)
         {
            mulle_fprintf( stderr, "%s: replacement is not valid UTF-8\n", argv[ 0]);
            mulle_utf32regex_free( regex);
            mulle_free( pattern);
            return( 1);
         }
      }

      memset( &regex_context, 0, sizeof( regex_context));
      regex_context.regex       = regex;
      regex_context.pattern     = pattern;
      regex_context.replacement = replacement;
      regex_context.counters    = &counters;

      mulle_buffer_init( &buffer, 0, NULL);
      if( read_input( &buffer, options.path))
      {
         mulle_buffer_done( &buffer);
         return( 1);
      }
      each_line( &buffer,
                 options.mode == mode_extract ? extract_line : rewrite_line,
                 &regex_context);
      mulle_buffer_done( &buffer);

      mulle_utf32regex_free( regex);
      mulle_free( pattern);
      mulle_free( replacement);
      return( 0);
   }

   mulle_buffer_init( &buffer, 0, NULL);
   if( read_input( &buffer, options.path))
   {
      mulle_buffer_done( &buffer);
      return( 1);
   }

   if( options.mode == mode_http)
   {
      rval = run_http( &buffer);
      mulle_buffer_done( &buffer);
      return( rval);
   }

   if( options.mode == mode_slug)
      each_line( &buffer, slug_line, &counters);
   else
      each_line( &buffer, report_url, &counters);

   print_summary( &counters);
   mulle_buffer_done( &buffer);
   return( 0);
}
