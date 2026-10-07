//
//  mredis.c
//  miniredis -- a small Redis-shaped server
//
//  Transport is a thread per connection, deliberately: the interesting part of
//  this demo is that N connection threads hammer one shared lock-free keyspace,
//  and per-connection ordering then comes for free, because one connection is
//  one thread. The cost is the usual one (a thread and a stack per client),
//  which is why real Redis does it the other way round: one thread, and
//  concurrency borrowed from the OS for the slow syscalls only.
//
//  --stdio serves the same protocol on stdin/stdout, which is what the tests
//  use: no ports, no threads, no timing, and the same parser and dispatcher as
//  the network path.
//

#include "store.h"
#include "resp.h"

#include <mulle-aba/mulle-aba.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/*
 * Networking is mulle-socket's job now, on every platform: there is no longer
 * a socket and a no-socket build, and --stdio is not a fallback for a platform
 * without one. Sockets arrive with mulle-core, which this includes.
 *
 * The CRT include is for --stdio alone, whose binary mode has to be set from
 * inside this process.
 */
#if defined( _WIN32)
# include <fcntl.h>
# include <io.h>
#else
/* unistd for the descriptor half of --stdio, signal for SIGPIPE below */
# include <signal.h>
# include <unistd.h>
#endif


#define MREDIS_VERSION   "0.1"

#define MREDIS_DEFAULT_PORT   6379
#define MREDIS_DEFAULT_BIND   "127.0.0.1"

#define MREDIS_READ_BUFFER    16384


/*
 * Where a connection's bytes come from. The two are not interchangeable: a
 * socket is a mulle_socket_t -- a SOCKET on Windows, which is a handle and not
 * a CRT descriptor, so a read() on one fails -- while --stdio reads the CRT's
 * own stdin/stdout. That makes it a runtime property of the connection, which
 * is why the build no longer decides it with a #define.
 */
enum mredis_transport
{
   MREDIS_TRANSPORT_STDIO = 0,
   MREDIS_TRANSPORT_SOCKET
};


struct mredis_connection
{
   enum mredis_transport  transport;  // sockets or stdin/stdout
   mulle_socket_t         fd;         // the socket, for MREDIS_TRANSPORT_SOCKET
   int                    verbose;
   int                    quit;
   struct mredis_store   *store;
   struct mredis_input    input;
   struct mredis_command  command;
   struct mulle_buffer    reply;
};


static int   g_verbose;


//
// small helpers
//
static void   mredis_add_literal( struct mulle_buffer *buffer, const char *text)
{
   mulle_buffer_add_bytes( buffer, text, strlen( text));
}


/*
 * ASCII case-insensitive compare against a literal command name. Deliberately
 * not strcasecmp(): command names are ASCII, and the locale's idea of case is
 * not always.
 */
static int   mredis_token_equals( const char *token, const char *literal)
{
   char   c;

   while( *literal)
   {
      c = *token;
      if( c >= 'A' && c <= 'Z')
         c = (char) (c + ('a' - 'A'));
      if( c != *literal)
         return( 0);
      token++;
      literal++;
   }
   return( *token == 0);
}


static void   mredis_reply_wrong_arity( struct mulle_buffer *buffer,
                                        const char *name,
                                        size_t name_length)
{
   mredis_add_literal( buffer, "-ERR wrong number of arguments for '");
   mulle_buffer_add_bytes( buffer, name, name_length);
   mredis_add_literal( buffer, "' command\r\n");
}


static void   mredis_reply_unknown( struct mulle_buffer *buffer,
                                    const char *name,
                                    size_t name_length)
{
   size_t   i;

   mredis_add_literal( buffer, "-ERR unknown command '");
   // The name came from the network, so it cannot be pasted in unchecked: a CR
   // or LF in it would end the reply early and hand the client the protocol.
   for( i = 0; i < name_length; i++)
   {
      char   c;

      c = name[ i];
      if( c != '\r' && c != '\n')
         mulle_buffer_add_bytes( buffer, &c, 1);
   }
   mredis_add_literal( buffer, "'\r\n");
}


//
// commands
//
static void   mredis_dispatch( struct mredis_connection *conn,
                               struct mredis_command *command)
{
   struct mulle_buffer   *reply;
   char                  *name;
   size_t                 name_length;

   reply       = &conn->reply;
   name        = command->argv[ 0];
   name_length = command->argv_length[ 0];

   if( mredis_token_equals( name, "ping"))
   {
      if( command->argc == 1)
         mredis_reply_simple( reply, "PONG");
      else
      {
         if( command->argc != 2)
         {
            mredis_reply_wrong_arity( reply, name, name_length);
            return;
         }
         mredis_reply_bulk( reply, command->argv[ 1], command->argv_length[ 1]);
      }
      return;
   }

   if( mredis_token_equals( name, "set"))
   {
      int   rval;

      if( command->argc != 3)
      {
         mredis_reply_wrong_arity( reply, name, name_length);
         return;
      }

      rval = mredis_store_set( conn->store,
                               command->argv[ 1], command->argv_length[ 1],
                               command->argv[ 2], command->argv_length[ 2]);
      if( rval == MREDIS_STORE_OK)
         mredis_reply_simple( reply, "OK");
      else if( rval == MREDIS_STORE_COLLISION)
         mredis_reply_error( reply, "ERR hash collision with a different key");
      else
         mredis_reply_error( reply, "ERR could not store the value");
      return;
   }

   if( mredis_token_equals( name, "get"))
   {
      struct mredis_reader   reader;
      struct mredis_value   *value;
      size_t                 length;

      if( command->argc != 2)
      {
         mredis_reply_wrong_arity( reply, name, name_length);
         return;
      }

      /*
       * The value is borrowed: it stays valid until reader_end(), even if a
       * concurrent SET replaces or deletes it meanwhile. That is the whole
       * point of the epoch, and it is why the reply has to be built before
       * the epoch is left.
       */
      mredis_store_reader_begin( &reader, conn->store);
      value = mredis_store_get( &reader,
                                command->argv[ 1], command->argv_length[ 1],
                                &length);
      if( value)
         mredis_reply_bulk( reply, mredis_value_bytes( value), length);
      else
         mredis_reply_null_bulk( reply);
      mredis_store_reader_end( &reader);
      return;
   }

   if( mredis_token_equals( name, "del") || mredis_token_equals( name, "exists"))
   {
      int        is_delete;
      intptr_t   count;
      size_t     i;

      if( command->argc < 2)
      {
         mredis_reply_wrong_arity( reply, name, name_length);
         return;
      }

      is_delete = mredis_token_equals( name, "del");
      count     = 0;
      for( i = 1; i < command->argc; i++)
      {
         if( is_delete)
            count += mredis_store_delete( conn->store,
                                          command->argv[ i],
                                          command->argv_length[ i]);
         else
            count += mredis_store_exists( conn->store,
                                          command->argv[ i],
                                          command->argv_length[ i]);
      }
      mredis_reply_integer( reply, count);
      return;
   }

   if( mredis_token_equals( name, "quit"))
   {
      if( command->argc != 1)
      {
         mredis_reply_wrong_arity( reply, name, name_length);
         return;
      }
      mredis_reply_simple( reply, "OK");
      conn->quit = 1;
      return;
   }

   mredis_reply_unknown( reply, name, name_length);
}


//
// connection
//
/*
 * --stdio, on both platforms, through the descriptors the CRT also gives the
 * streams: 0 and 1 are stdin and stdout there too. They are used rather than
 * the FILE * because stdout is block buffered when it is not a terminal, and a
 * protocol that waits for a reply would wait for a buffer that has not filled.
 */
static intptr_t   mredis_stdio_read( void *buffer, size_t length)
{
   ssize_t   n;

   for(;;)
   {
#if defined( _WIN32)
      n = _read( 0, buffer, (unsigned int) length);
#else
      n = read( 0, buffer, length);
#endif
      if( n >= 0 || mulle_socket_error() != EINTR)
         return( (intptr_t) n);
   }
}


static int   mredis_stdio_write_all( void const *bytes, size_t length)
{
   char       *p;
   size_t      written;
   intptr_t    n;

   p       = (char *) bytes;
   written = 0;

   while( written < length)
   {
#if defined( _WIN32)
      n = _write( 1, p + written, (unsigned int) (length - written));
#else
      n = write( 1, p + written, length - written);
#endif
      if( n > 0)
      {
         written += (size_t) n;
         continue;
      }
      if( n < 0 && mulle_socket_error() == EINTR)
         continue;
      return( -1);
   }
   return( 0);
}


static int   mredis_connection_flush( struct mredis_connection *conn)
{
   char     *bytes;
   size_t    length;
   int       rval;

   bytes  = mulle_buffer_get_bytes( &conn->reply);
   length = mulle_buffer_get_length( &conn->reply);
   if( ! length)
      return( 0);

   if( conn->transport == MREDIS_TRANSPORT_SOCKET)
      rval = (int) mulle_socket_write_all( conn->fd, bytes, length);
   else
      rval = mredis_stdio_write_all( bytes, length);

   mulle_buffer_reset( &conn->reply);
   return( rval);
}


static int   mredis_connection_read( struct mredis_connection *conn)
{
   char        buffer[ MREDIS_READ_BUFFER];
   intptr_t    n;

   for(;;)
   {
      if( conn->transport == MREDIS_TRANSPORT_SOCKET)
      {
         /* EINTR is already retried in here */
         n = mulle_socket_read( conn->fd, buffer, sizeof( buffer));
      }
      else
         n = mredis_stdio_read( buffer, sizeof( buffer));

      if( n > 0)
         return( mredis_input_add( &conn->input, buffer, (size_t) n) ? -1 : 1);
      if( ! n)
         return( 0);

      /* the socket half has already given up by now */
      if( conn->transport == MREDIS_TRANSPORT_SOCKET ||
          mulle_socket_error() != EINTR)
         return( -1);
   }
}


static void   mredis_connection_serve( struct mredis_connection *conn)
{
   int   rval;

   for(;;)
   {
      /*
       * Drain everything the buffer already holds before going back to the
       * socket: a pipelining client then gets one write for the whole batch
       * instead of one write per command.
       */
      while( (rval = mredis_input_read_command( &conn->input, &conn->command)) == 1)
      {
         if( ! conn->command.argc)
            continue;   // `*0`, `*-1`, or an empty inline line

         mredis_dispatch( conn, &conn->command);
         if( conn->quit)
            break;
      }

      if( rval < 0)
      {
         mredis_add_literal( &conn->reply, "-ERR Protocol error: ");
         // input->error is a literal, never client data.
         mredis_add_literal( &conn->reply, conn->input.error ? conn->input.error : "bad request");
         mredis_add_literal( &conn->reply, "\r\n");
         (void) mredis_connection_flush( conn);
         break;
      }

      if( mredis_connection_flush( conn))
         break;

      if( conn->quit)
         break;

      if( mredis_connection_read( conn) <= 0)
         break;
   }
}


static mulle_thread_rval_t   mredis_connection_main( void *arg)
{
   struct mredis_connection   *conn;

   conn = (struct mredis_connection *) arg;

   /*
    * mulle-aba requires this in every thread that touches a concurrent
    * container: the deferred frees it performs are per-thread bookkeeping. A
    * thread that is not registered has no place to check retirement into, and
    * the keyspace's own allocator hook expects one.
    */
   mulle_aba_register();

   mredis_connection_serve( conn);

   if( g_verbose)
      mulle_fprintf( stderr, "mredis: connection closed\n");

   mulle_aba_unregister();

   mredis_input_done( &conn->input);
   mulle_buffer_done( &conn->reply);
   mulle_socket_close( conn->fd);
   mulle_free( conn);
   return( 0);
}


static void   mredis_connection_init( struct mredis_connection *conn,
                                      struct mredis_store *store,
                                      enum mredis_transport transport,
                                      mulle_socket_t fd)
{
   memset( conn, 0, sizeof( struct mredis_connection));
   conn->transport = transport;
   conn->fd        = fd;
   conn->store     = store;
   conn->verbose   = g_verbose;
   mredis_input_init( &conn->input);
   mulle_buffer_init( &conn->reply, 0, NULL);
}


static void   mredis_connection_done( struct mredis_connection *conn)
{
   mredis_input_done( &conn->input);
   mulle_buffer_done( &conn->reply);
}


//
// main
//
/*
 * Decimal into a caller supplied buffer. The mulle formatter's conversion
 * table is not documented anywhere, so every number this demo
 * prints goes through here and is passed on as a %s.
 */
static char   *mredis_uint_string( char *buffer, size_t size, unsigned int value)
{
   size_t   i;

   i = size;
   buffer[ --i] = 0;
   do
   {
      buffer[ --i] = (char) ('0' + (value % 10));
      value        /= 10;
   }
   while( value);

   return( &buffer[ i]);
}


static void   mredis_usage( FILE *fp)
{
   char   port[ 16];

   mulle_fprintf( fp, "usage: mredis [options]\n");
   mulle_fprintf( fp, "\n");
   mulle_fprintf( fp, "  --port N       listen on this port (0 picks a free one, default %s)\n",
                  mredis_uint_string( port, sizeof( port), MREDIS_DEFAULT_PORT));
   mulle_fprintf( fp, "  --bind ADDR    bind address (default %s)\n", MREDIS_DEFAULT_BIND);
   mulle_fprintf( fp, "  --stdio        serve one client on stdin/stdout instead\n");
   mulle_fprintf( fp, "  -v, --verbose  log connections and shutdowns\n");
   mulle_fprintf( fp, "  -h, --help     this text\n");
   mulle_fprintf( fp, "\n");
   mulle_fprintf( fp, "Commands: PING, SET, GET, DEL, EXISTS, QUIT\n");
   mulle_fprintf( fp, "\n");
   mulle_fprintf( fp, "The banner with the actual port goes to stderr, so that --stdio\n");
   mulle_fprintf( fp, "keeps stdout clean for the protocol.\n");
}


static void   mredis_banner( const char *address, unsigned int port)
{
   char   buffer[ 16];

   mulle_fprintf( stderr, "mredis %s: listening on %s:%s\n",
                  MREDIS_VERSION,
                  address,
                  mredis_uint_string( buffer, sizeof( buffer), port));
}


/*
 * The demo's `fake-redis-server` stand-in is this program with redis-server's
 * command line: it maps the arguments and calls the entry point below, in this
 * same process. That is the whole point of it living in here rather than in a
 * script or a wrapper -- the server the benchmark measures then has the pid the
 * benchmark started, so the RSS and CPU beside it are the server's. A script
 * gets away with that on POSIX by `exec`ing, and Windows has no `exec`.
 *
 * Built as a target of its own with -DMREDIS_FIXTURE_MAIN, the entry point is
 * named so the stand-in can call it; `mredis` itself is unchanged.
 */
#ifdef MREDIS_FIXTURE_MAIN
# define main   mredis_server_main
#endif


int   main( int argc, char *argv[])
{
   struct mredis_store       *store;
   struct mredis_connection   conn;
   const char                *address;
   unsigned int               port;
   int                        use_stdio;
   int                        i;
   mulle_socket_t             listen_fd;

   address   = MREDIS_DEFAULT_BIND;
   port      = MREDIS_DEFAULT_PORT;
   use_stdio = 0;

   for( i = 1; i < argc; i++)
   {
      if( ! strcmp( argv[ i], "-h") || ! strcmp( argv[ i], "--help"))
      {
         mredis_usage( stdout);
         return( 0);
      }
      if( ! strcmp( argv[ i], "-v") || ! strcmp( argv[ i], "--verbose"))
      {
         g_verbose = 1;
         continue;
      }
      if( ! strcmp( argv[ i], "--stdio"))
      {
         use_stdio = 1;
         continue;
      }
      if( ! strcmp( argv[ i], "--port") && i + 1 < argc)
      {
         port = (unsigned int) strtoul( argv[ ++i], NULL, 10);
         continue;
      }
      if( ! strcmp( argv[ i], "--bind") && i + 1 < argc)
      {
         address = argv[ ++i];
         continue;
      }
      mulle_fprintf( stderr, "mredis: unknown option %s\n", argv[ i]);
      mredis_usage( stderr);
      return( 1);
   }

   // Before the store: mredis_store_create() hands the table an ABA-aware
   // allocator, which needs the global mulle-aba to exist.
   mulle_aba_init( NULL);
   mulle_aba_register();

   store = mredis_store_create( MREDIS_STORE_DEFAULT_BUCKETS);
   if( ! store)
   {
      mulle_fprintf( stderr, "mredis: out of memory\n");
      return( 1);
   }

   if( use_stdio)
   {
#ifdef _WIN32
      /*
       * RESP has its own idea of line endings -- and its own lengths -- and a
       * CRT in text mode would rewrite the former and so corrupt the latter:
       * every \r\n on the wire would arrive as \r\r\n. The translation happens
       * inside this process, on this process's streams, which is why it has to
       * be said here and not by whoever started us.
       */
      (void) _setmode( _fileno( stdin), _O_BINARY);
      (void) _setmode( _fileno( stdout), _O_BINARY);
#endif

      mredis_connection_init( &conn, store, MREDIS_TRANSPORT_STDIO,
                              MULLE_SOCKET_INVALID);
      mredis_connection_serve( &conn);
      mredis_connection_done( &conn);
      mredis_store_destroy( store);
      mulle_aba_unregister();
      mulle_aba_done();
      return( 0);
   }

#if ! defined( _WIN32)
   /*
    * Still here, and not for the sockets: mulle-socket stops a write to a
    * departed peer from raising SIGPIPE, but --stdio writes to a *pipe*, and a
    * pipe whose reader is gone raises it on the writer the old way. A --stdio
    * client that exits gives us EOF, which is a clean stop; one that exits
    * mid-reply should not take the server down with it.
    */
   signal( SIGPIPE, SIG_IGN);
#endif

   // --port 0 means "any port": mulle_socket_listen reports back the one the
   // kernel picked, so a script can find it without guessing.
   listen_fd = mulle_socket_listen( address, port, &port);
   if( listen_fd == MULLE_SOCKET_INVALID)
   {
      mulle_fprintf( stderr, "mredis: cannot listen: %s\n",
                     strerror( mulle_socket_error()));
      mredis_store_destroy( store);
      return( 1);
   }

   mredis_banner( address, port);

   for(;;)
   {
      struct mredis_connection   *client;
      mulle_thread_t              thread;
      mulle_socket_t              fd;

      fd = mulle_socket_accept( listen_fd);
      if( fd == MULLE_SOCKET_INVALID)
      {
         /* EINTR is retried inside the library, and ECONNABORTED is a client
            that went away between the handshake and the accept: both are
            reasons to go round again, not to stop serving. */
         if( mulle_socket_error() == ECONNABORTED)
            continue;
         mulle_fprintf( stderr, "mredis: accept failed: %s\n",
                        strerror( mulle_socket_error()));
         break;
      }

      /* No TCP_NODELAY here: mulle_socket_accept sets it on every connection
         it hands out, and mulle-socket.h carries the pipelining measurement
         that says why it has to. */

      client = mulle_malloc( sizeof( struct mredis_connection));
      if( ! client)
      {
         mulle_socket_close( fd);
         continue;
      }
      mredis_connection_init( client, store, MREDIS_TRANSPORT_SOCKET, fd);

      if( mulle_thread_create( mredis_connection_main, client, &thread))
      {
         // Out of threads: serve it on this one rather than dropping it.
         mulle_fprintf( stderr, "mredis: could not spawn a thread, serving inline\n");
         mredis_connection_serve( client);
         mredis_connection_done( client);
         mulle_socket_close( fd);
         mulle_free( client);
         continue;
      }

      if( g_verbose)
         mulle_fprintf( stderr, "mredis: connection accepted\n");

      (void) mulle_thread_detach( thread);
   }

   mulle_socket_close( listen_fd);
   mredis_store_destroy( store);
   mulle_aba_unregister();
   mulle_aba_done();
   return( 0);
}
