//
//  client.c
//  miniredis
//

#include "client.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>

#if MREDIS_HAVE_POSIX
# include <dirent.h>
# include <fcntl.h>
# include <signal.h>
# include <sys/wait.h>
# include <unistd.h>
# if defined( MREDIS_HAVE_LIBPROC)
#  include <libproc.h>          // proc_listpids(), proc_pidinfo(), proc_pidfdinfo()
#  include <sys/proc_info.h>    // struct socket_fdinfo, PROX_FDTYPE_SOCKET, INI_IPV4
# endif
#endif

#if defined( MREDIS_HAVE_WINDOWS_ACCOUNTING)
/*
 * mulle-socket.h has already brought in winsock2.h, ws2tcpip.h and windows.h,
 * in that order, which is the order iphlpapi.h needs -- it wants winsock2.h and
 * is happy to follow windows.h.
 */
# include <iphlpapi.h>        // GetExtendedTcpTable(), MIB_TCPTABLE_OWNER_PID
#endif


/*
 * Decimals are built by hand rather than by the formatter, whose conversion
 * table is not documented. `append_uint` lays the digits down
 * *forwards* from `p` and returns the new end.
 *
 * This used to be a helper that filled a scratch buffer backwards and returned
 * a pointer into its middle, with the length computed as `returned - scratch`.
 * That length then spanned the uninitialized heading of the scratch, so every
 * length prefix went out as 19 bytes of stack garbage followed by the digit.
 * The server's answer was `-ERR Protocol error: invalid multibulk length`.
 * Filling forwards and returning the end makes the length exact by
 * construction.
 */
static char   *append_uint( char *p, uint64_t value)
{
   char     tmp[ 20];
   size_t   i;

   i = sizeof( tmp);
   do
   {
      tmp[ --i] = (char) ('0' + (value % 10));
      value    /= 10;
   }
   while( value);

   while( i < sizeof( tmp))
      *p++ = tmp[ i++];
   return( p);
}


static void   add_uint( struct mulle_buffer *buffer, uint64_t value)
{
   char     tmp[ 20];

   mulle_buffer_add_bytes( buffer, tmp,
                           (size_t) (append_uint( tmp, value) - tmp));
}


void   mredis_wire_init( struct mredis_wire *wire, int fd)
{
   memset( wire, 0, sizeof( struct mredis_wire));
   wire->transport = MREDIS_TRANSPORT_DESCRIPTOR;
   wire->fd        = fd;
   wire->socket    = MULLE_SOCKET_INVALID;
}


void   mredis_wire_init_socket( struct mredis_wire *wire, mulle_socket_t socket)
{
   memset( wire, 0, sizeof( struct mredis_wire));
   wire->transport = MREDIS_TRANSPORT_SOCKET;
   wire->socket    = socket;
   wire->fd        = -1;
}


/*
 * One read, however the wire was opened: a socket through mulle-socket, a pipe
 * end or stdin through the CRT. EINTR is retried for the descriptor half only,
 * because mulle_socket_read() has already done it.
 */
static intptr_t   mredis_wire_read_in( struct mredis_wire *wire,
                                       void *buffer,
                                       size_t length)
{
   for(;;)
   {
      if( wire->transport == MREDIS_TRANSPORT_SOCKET)
         return( mulle_socket_read( wire->socket, buffer, length));

      {
         intptr_t   n;

         n = mredis_read( wire->fd, buffer, length);
         if( n >= 0 || errno != EINTR)
            return( n);
      }
   }
}


static int   mredis_wire_fill( struct mredis_wire *wire)
{
   intptr_t   n;

   n = mredis_wire_read_in( wire, wire->buffer, sizeof( wire->buffer));
   if( n > 0)
   {
      wire->length      = (size_t) n;
      wire->offset      = 0;
      wire->bytes_read += (unsigned long long) n;
      return( 1);
   }
   if( ! n)
      return( 0);
   return( -1);
}


int   mredis_wire_getc( struct mredis_wire *wire)
{
   if( wire->offset == wire->length)
   {
      if( mredis_wire_fill( wire) <= 0)
         return( -1);
   }
   return( (unsigned char) wire->buffer[ wire->offset++]);
}


int   mredis_wire_read( struct mredis_wire *wire, char *dst, size_t length)
{
   size_t    got;

   got = 0;
   while( got < length)
   {
      if( wire->offset < wire->length)
      {
         size_t   available;

         available = wire->length - wire->offset;
         if( available > length - got)
            available = length - got;
         memcpy( &dst[ got], &wire->buffer[ wire->offset], available);
         wire->offset += available;
         got          += available;
         continue;
      }

      {
         intptr_t   n;

         n = mredis_wire_read_in( wire, &dst[ got], length - got);
         if( n > 0)
         {
            got              += (size_t) n;
            wire->bytes_read += (unsigned long long) n;
            continue;
         }
      }
      return( -1);
   }
   return( 0);
}


/* one CRLF terminated line, without the CRLF */
int   mredis_wire_read_line( struct mredis_wire *wire, char *dst, size_t size)
{
   size_t   i;
   int      c;

   i = 0;
   for(;;)
   {
      c = mredis_wire_getc( wire);
      if( c < 0)
         return( -1);
      if( c == '\n')
         break;
      if( i + 1 < size)
         dst[ i++] = (char) c;
   }
   dst[ i] = 0;
   if( i && dst[ i - 1] == '\r')
      dst[ i - 1] = 0;
   return( 0);
}


int   mredis_wire_read_reply( struct mredis_wire *wire,
                              struct mredis_reply *reply,
                              char *value,
                              size_t value_size)
{
   char     line[ 64];
   int      c;

   reply->integer  = 0;
   reply->length   = 0;
   reply->children = 0;

   c = mredis_wire_getc( wire);
   if( c < 0)
   {
      reply->type = MREDIS_REPLY_NONE;
      return( -1);
   }

   switch( c)
   {
   case '+' :
      reply->type = MREDIS_REPLY_SIMPLE;
      return( mredis_wire_read_line( wire, value, value_size));

   case '-' :
      reply->type = MREDIS_REPLY_ERROR;
      return( mredis_wire_read_line( wire, value, value_size));

   case ':' :
      if( mredis_wire_read_line( wire, line, sizeof( line)))
         return( -1);
      reply->type    = MREDIS_REPLY_INTEGER;
      reply->integer = strtoll( line, NULL, 10);
      return( 0);

   case '$' :
      if( mredis_wire_read_line( wire, line, sizeof( line)))
         return( -1);

      reply->length = (size_t) strtoll( line, NULL, 10);
      if( (long long) reply->length < 0)     // $-1\r\n: no value
      {
         reply->type   = MREDIS_REPLY_NULL;
         reply->length = 0;
         return( 0);
      }
      if( reply->length + 1 > value_size)
      {
         reply->type = MREDIS_REPLY_BROKEN;
         return( -1);
      }

      reply->type = MREDIS_REPLY_BULK;
      if( mredis_wire_read( wire, value, reply->length))
         return( -1);
      if( mredis_wire_read( wire, value + reply->length, 2))
         return( -1);
      value[ reply->length] = 0;
      return( 0);

   case '*' :
      if( mredis_wire_read_line( wire, line, sizeof( line)))
         return( -1);
      reply->type     = MREDIS_REPLY_ARRAY;
      reply->children = (size_t) strtoll( line, NULL, 10);
      return( 0);

   default :
      reply->type = MREDIS_REPLY_BROKEN;
      return( -1);
   }
}


void   mredis_encode_argv( struct mulle_buffer *buffer,
                           size_t argc,
                           const char *const *argv,
                           const size_t *argv_length)
{
   size_t   i;

   mulle_buffer_add_bytes( buffer, "*", 1);
   add_uint( buffer, (uint64_t) argc);
   mulle_buffer_add_bytes( buffer, "\r\n", 2);

   for( i = 0; i < argc; i++)
   {
      mulle_buffer_add_bytes( buffer, "$", 1);
      add_uint( buffer, (uint64_t) argv_length[ i]);
      mulle_buffer_add_bytes( buffer, "\r\n", 2);
      mulle_buffer_add_bytes( buffer, argv[ i], argv_length[ i]);
      mulle_buffer_add_bytes( buffer, "\r\n", 2);
   }
}


void   mredis_encode_command( const char *line, struct mulle_buffer *buffer)
{
   const char   *argv[ 8];
   size_t        argv_length[ 8];
   const char   *p;
   size_t        argc;

   argc = 0;
   p    = line;
   while( *p && argc < 8)
   {
      while( *p == ' ' || *p == '\t')
         p++;
      if( ! *p)
         break;

      if( *p == '"')
      {
         p++;
         argv[ argc] = p;
         while( *p && *p != '"')
            p++;
         argv_length[ argc] = (size_t) (p - argv[ argc]);
         if( *p == '"')
            p++;
      }
      else
      {
         argv[ argc] = p;
         while( *p && *p != ' ' && *p != '\t')
            p++;
         argv_length[ argc] = (size_t) (p - argv[ argc]);
      }
      argc++;
   }

   mredis_encode_argv( buffer, argc, argv, argv_length);
}


int   mredis_write_all( int fd, const void *bytes, size_t length)
{
   const char   *p;
   size_t        written;
   int           n;

   p       = bytes;
   written = 0;
   while( written < length)
   {
      n = mredis_write( fd, &p[ written], length - written);
      if( n > 0)
      {
         written += (size_t) n;
         continue;
      }
      if( n < 0 && errno == EINTR)
         continue;
      return( -1);
   }
   return( 0);
}


/*
 * A server on stdin/stdout. This is the whole transport where there is no
 * network, and it is the same two pipes everywhere -- which is why it is not
 * inside the sockets guard.
 *
 * The two ends come back in the handle: `handle->in` is written to and
 * `handle->out` is read from, and closing `handle->in` is how the server is told
 * there is no more.
 */
int   mredis_spawn_stdio( const char *server, struct mulle_spawn_handle *handle)
{
   char   *argv[ 3];

   argv[ 0] = (char *) server;
   argv[ 1] = "--stdio";
   argv[ 2] = NULL;

   if( mulle_spawn_open( handle, argv, MULLE_SPAWN_PIPE, MULLE_SPAWN_PIPE,
                         MULLE_SPAWN_INHERIT))
   {
      mulle_fprintf( stderr, "mredis: cannot start %s: %s\n",
                     server, strerror( errno));
      return( -1);
   }
   return( 0);
}


/*
 * A server on an ephemeral port, which it announces on stderr. So the banner is
 * read from the child's stderr *while the child keeps running*, and only then is
 * the handle handed back. The banner is
 *
 *   mredis 0.1: listening on 127.0.0.1:<port>
 *
 * and the stderr pipe is closed once it has been read, because that is the last
 * thing this server says on it. A server that later logs would find the reader
 * gone; `mredis_spawn_tcp_port` is the helper for servers that do.
 */
int   mredis_spawn_server( const char *server,
                           unsigned int *p_port,
                           struct mulle_spawn_handle *handle)
{
   char          *argv[ 6];
   char          *colon;
   char          line[ 256];
   unsigned int  port;
   int           i;
   int           n;

   argv[ 0] = (char *) server;
   argv[ 1] = "--port";
   argv[ 2] = "0";
   argv[ 3] = "--bind";
   argv[ 4] = "127.0.0.1";
   argv[ 5] = NULL;

   if( mulle_spawn_open( handle, argv, MULLE_SPAWN_INHERIT, MULLE_SPAWN_INHERIT,
                         MULLE_SPAWN_PIPE))
      return( -1);

   i = 0;
   for(;;)
   {
      n = (int) mredis_read( handle->err, &line[ i], 1);
      if( n < 0 && errno == EINTR)
         continue;
      if( n <= 0)
      {
         mulle_fprintf( stderr,
                        "mredis: the server did not announce a port (did it start?)\n");
         mulle_spawn_close( handle, NULL);
         return( -1);
      }
      if( line[ i] == '\n' || i == (int) sizeof( line) - 1)
         break;
      i++;
   }
   line[ i] = 0;

   colon = strrchr( line, ':');
   if( ! colon)
   {
      mulle_fprintf( stderr, "mredis: cannot parse the banner: %s\n", line);
      mulle_spawn_close( handle, NULL);
      return( -1);
   }

   port = (unsigned int) strtoul( colon + 1, NULL, 10);
   if( ! port)
   {
      mulle_fprintf( stderr, "mredis: the banner carries no port: %s\n", line);
      mulle_spawn_close( handle, NULL);
      return( -1);
   }

   *p_port = port;

   mredis_close( handle->err);
   handle->err = -1;
   return( 0);
}


/*
 * One call to mulle-socket. TCP_NODELAY, the batch tail and the Nagle timer
 * all live in the library now, at the point where it sets the option, which is
 * why nothing here touches a socket option any more.
 */
mulle_socket_t   mredis_connect( const char *host, unsigned int port)
{
   return( mulle_socket_connect( host, port));
}


/*
 * Spawn `mredis --port 0` and read the port it announces on stderr. The banner
 * is "mredis 0.1: listening on 127.0.0.1:<port>".
 */
/*
 * Pick a port nobody is listening on. There is an unavoidable race between the
 * check and the server binding it; the caller treats a failure to connect as
 * fatal rather than retrying, so a lost race is loud instead of silent.
 */
int   mredis_find_free_port( unsigned int *p_port)
{
   unsigned int   port;

   port = mulle_socket_find_free_port();
   if( ! port)
      return( -1);

   *p_port = port;
   return( 0);
}


/*
 * Start any Redis-compatible server on a port we picked. Not mredis's banner
 * dance: redis-server announces nothing machine readable on a chosen port, so
 * this waits for the port to accept a connection instead.
 */
int   mredis_spawn_tcp_port( const char *server,
                             const char *const *extra_argv,
                             unsigned int *p_port,
                             struct mulle_spawn_handle *handle)
{
   char           port_text[ 16];
   const char    *argv[ 16];
   unsigned int   port;
   int            i;
   int            n;

   if( mredis_find_free_port( &port))
      return( -1);

   snprintf( port_text, sizeof( port_text), "%u", port);

   i        = 0;
   argv[ i++] = server;
   argv[ i++] = "--port";
   argv[ i++] = port_text;
   argv[ i++] = "--bind";
   argv[ i++] = "127.0.0.1";
   while( extra_argv && *extra_argv && i < 14)
      argv[ i++] = *extra_argv++;
   argv[ i] = NULL;

   /*
    * A real server logs its startup to stdout, which is the terminal this
    * benchmark is drawing on. Nothing here needs to see it.
    */
   if( mulle_spawn_open( handle, (char * const *) argv, MULLE_SPAWN_INHERIT,
                         MULLE_SPAWN_DISCARD, MULLE_SPAWN_INHERIT))
      return( -1);

   /* wait for it to answer, up to ~5 s */
   for( n = 0; n < 500; n++)
   {
      mulle_socket_t   fd;

      fd = mredis_connect( "127.0.0.1", port);
      if( fd != MULLE_SOCKET_INVALID)
      {
         mulle_socket_close( fd);
         *p_port = port;
         return( 0);
      }

      mulle_relativetime_sleep( 0.01);
   }

   mulle_spawn_close( handle, NULL);
   return( -1);
}


/*
 * Which process is listening on this port?
 *
 * Linux gets there from the table side: /proc/net/tcp lists the listening
 * socket with its inode, and /proc/<pid>/fd points at "socket:[inode]". That
 * is enough to read someone else's VmRSS without asking them.
 *
 * macOS gets there from the descriptor side, because macOS keeps no table with
 * a pid in it -- the port lives in the socket, and the socket lives in some
 * process's descriptor table, so libproc has to be asked process by process.
 *
 * Windows gets there from the table side too, and in one call: its TCP table
 * carries the owning pid, so there is no inode to chase. Whether that process
 * can then be *read* is OpenProcess's answer, which is the same refusal /proc
 * and libproc give -- a lookup is a debugging convenience, not a way around
 * permissions.
 */
#if defined( MREDIS_HAVE_PROC)

//
// The inode of the listening socket for `port`, from one of the
// /proc/net/tcp{,6} tables, or 0 if it is not there.
//
// The layout is:
//
//   sl local_address rem_address st tx_queue:rx_queue tr tm->when
//   retrnsmt uid timeout inode
//
// so after `st` there are five fields to walk past before the inode: skipping
// four lands on `timeout`, which is 0 for a listener. Hence the `sock_inode`
// check -- a 0 there means the line was not shaped as expected, and a wrong
// inode would be worse than none, because it would match somebody else's fd.
//
static unsigned long   mredis_inode_for_port_in( const char *path,
                                                 unsigned int port)
{
   unsigned long   found_inode;
   char            line[ 512];
   FILE           *fp;

   fp = fopen( path, "r");
   if( ! fp)
      return( 0);

   if( ! fgets( line, sizeof( line), fp))    /* header */
   {
      fclose( fp);
      return( 0);
   }

   found_inode = 0;
   while( fgets( line, sizeof( line), fp))
   {
      unsigned int    local_port;
      unsigned long   sock_inode;
      unsigned int    state;
      char            local[ 64];
      char           *colon;

      if( sscanf( line, " %*d: %63s %*s %x %*s %*s %*s %*s %*s %lu",
                  local, &state, &sock_inode) != 3)
         continue;

      if( state != 0x0a)          /* 0A: LISTEN */
         continue;

      colon = strrchr( local, ':');
      if( ! colon)
         continue;

      local_port = (unsigned int) strtoul( colon + 1, NULL, 16);
      if( local_port != port)
         continue;

      if( sock_inode)
      {
         found_inode = sock_inode;
         break;
      }
   }
   fclose( fp);
   return( found_inode);
}


long   mredis_pid_for_port( unsigned int port)
{
   unsigned long   found_inode;
   char            needle[ 64];
   DIR            *dir;
   struct dirent  *entry;
   long            pid;
   int             done;

   found_inode = mredis_inode_for_port_in( "/proc/net/tcp", port);
   if( ! found_inode)
      found_inode = mredis_inode_for_port_in( "/proc/net/tcp6", port);

   if( ! found_inode)
      return( -1);

   snprintf( needle, sizeof( needle), "socket:[%lu]", found_inode);

   dir = opendir( "/proc");
   if( ! dir)
      return( -1);

   pid  = -1;
   done = 0;
   while( ! done && (entry = readdir( dir)))
   {
      char   path[ 64];
      DIR   *fds;

      if( entry->d_name[ 0] < '0' || entry->d_name[ 0] > '9')
         continue;

      pid = strtol( entry->d_name, NULL, 10);
      snprintf( path, sizeof( path), "/proc/%ld/fd", pid);

      fds = opendir( path);
      if( ! fds)
         continue;

      for(;;)
      {
         struct dirent   *fde;
         char             link[ 512];
         char             target[ 256];
         ssize_t          length;

         fde = readdir( fds);
         if( ! fde)
            break;

         if( snprintf( link, sizeof( link), "%s/%s", path, fde->d_name) >= (int) sizeof( link))
            continue;
         length = readlink( link, target, sizeof( target) - 1);
         if( length <= 0)
            continue;
         target[ length] = 0;

         if( ! strcmp( target, needle))
         {
            done = 1;
            break;
         }
      }
      closedir( fds);
      if( done)
         break;
      pid = -1;
   }
   closedir( dir);
   return( pid);
}

#elif defined( MREDIS_HAVE_LIBPROC)

/*
 * `tcp_sockinfo.tcpsi_state` is the TCP state machine's state, so LISTEN is 1
 * whichever name it arrives under: some SDKs spell it TSI_S_LISTEN and others
 * leave it to <netinet/tcp_fsm.h>, and this file should not depend on which.
 */
#if defined( TSI_S_LISTEN)
# define MREDIS_TSI_LISTEN   TSI_S_LISTEN
#else
# define MREDIS_TSI_LISTEN   1
#endif


/*
 * The listening TCP socket on `port` among one process's descriptors, or -1.
 *
 * PROC_PIDLISTFDS answers with the descriptors and their types, then
 * PROC_PIDFDSOCKETINFO answers with each socket's own protocol information.
 * A descriptor this user may not inspect simply does not report, which is the
 * same answer /proc gives on Linux: the lookup is a debugging convenience, not
 * a way around permissions.
 */
static long   mredis_pid_listening_on( pid_t pid, unsigned int port)
{
   struct proc_fdinfo   fds[ 1024];
   int                  length;
   int                  nfds;
   int                  i;

   length = proc_pidinfo( pid, PROC_PIDLISTFDS, 0, fds, (int) sizeof( fds));
   if( length <= 0)
      return( -1);

   nfds = length / (int) sizeof( struct proc_fdinfo);
   for( i = 0; i < nfds; i++)
   {
      struct socket_fdinfo   info;
      struct in_sockinfo    *local;

      if( fds[ i].proc_fdtype != PROX_FDTYPE_SOCKET)
         continue;

      if( proc_pidfdinfo( pid, fds[ i].proc_fd, PROC_PIDFDSOCKETINFO,
                          &info, (int) sizeof( info)) != (int) sizeof( info))
         continue;

      if( info.psi.soi_kind != SOCKINFO_TCP)
         continue;

      if( info.psi.soi_proto.pri_tcp.tcpsi_state != MREDIS_TSI_LISTEN)
         continue;

      local = &info.psi.soi_proto.pri_tcp.tcpsi_ini;
      if( ! (local->insi_vflag & INI_IPV4))
         continue;

      if( (unsigned int) ntohs( (uint16_t) local->insi_lport) == port)
         return( (long) pid);
   }
   return( -1);
}


long   mredis_pid_for_port( unsigned int port)
{
   pid_t   *pids;
   long     found;
   int      length;
   int      count;
   int      i;

   /*
    * proc_listpids() answers in bytes both times: first how much room every
    * process needs, then how many were written into the buffer. The spare
    * pid_t in the allocation is for the processes that appear between those
    * two calls -- which is the normal case, not an error.
    */
   length = proc_listpids( PROC_ALL_PIDS, 0, NULL, 0);
   if( length <= 0)
      return( -1);

   pids = mulle_malloc( (size_t) length + sizeof( pid_t));
   if( ! pids)
      return( -1);

   count = proc_listpids( PROC_ALL_PIDS, 0, pids, length);
   if( count <= 0)
   {
      mulle_free( pids);
      return( -1);
   }

   found = -1;
   count /= (int) sizeof( pid_t);
   for( i = 0; i < count && found < 0; i++)
      found = mredis_pid_listening_on( pids[ i], port);

   mulle_free( pids);
   return( found);
}

#elif defined( MREDIS_HAVE_WINDOWS_ACCOUNTING)

/*
 * One call, and no inode: the TCP table carries the owning pid, so the port and
 * the process come back together. Asked twice -- once for the size, once for the
 * rows -- which is the shape every iphlpapi table call has.
 *
 * TCP_TABLE_OWNER_PID_LISTENER is the listener-only table, so a socket in any
 * other state is simply not in it. IPv4 only: every server this reaches is
 * bound to 127.0.0.1, and a v6 listener answers -1 rather than a wrong pid.
 */
long   mredis_pid_for_port( unsigned int port)
{
   MIB_TCPTABLE_OWNER_PID   *table;
   DWORD                     length;
   DWORD                     rows;
   DWORD                     i;
   long                      pid;

   length = 0;
   if( GetExtendedTcpTable( NULL, &length, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
      return( -1);

   table = mulle_malloc( (size_t) length);
   if( ! table)
      return( -1);

   if( GetExtendedTcpTable( table, &length, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
   {
      mulle_free( table);
      return( -1);
   }

   pid  = -1;
   rows = table->dwNumEntries;
   for( i = 0; i < rows; i++)
   {
      /* the port sits in the low half of the field, in network order */
      if( ntohs( (unsigned short) table->table[ i].dwLocalPort) == (unsigned short) port)
      {
         pid = (long) table->table[ i].dwOwningPid;
         break;
      }
   }

   mulle_free( table);
   return( pid);
}

#else

/*
 * No /proc, no libproc and no TCP table: a platform that keeps its process
 * accounting somewhere else entirely. It says so instead of inventing a number.
 */
long   mredis_pid_for_port( unsigned int port)
{
   MULLE_C_UNUSED( port);
   return( -1);
}

#endif


#if MREDIS_HAVE_POSIX

mulle_socket_t   mredis_connect_unix( const char *path)
{
   return( mulle_socket_connect_unix( path));
}

#else

/*
 * mulle-socket does not offer AF_UNIX on Windows -- the SDK question is not one
 * it wants to answer -- so this is the refusal. It lives here rather than at
 * every call site because "this platform has no unix socket" is a property of
 * the platform.
 */
mulle_socket_t   mredis_connect_unix( const char *path)
{
   MULLE_C_UNUSED( path);
   errno = ENOTSUP;
   return( MULLE_SOCKET_INVALID);
}

#endif


int   mredis_spawn_tcp( const char *server,
                        mulle_socket_t *p_fd,
                        struct mulle_spawn_handle *handle)
{
   unsigned int     port;
   mulle_socket_t   fd;

   if( mredis_spawn_server( server, &port, handle))
      return( -1);

   fd = mredis_connect( "127.0.0.1", port);
   if( fd == MULLE_SOCKET_INVALID)
   {
      mulle_fprintf( stderr, "mredis: cannot connect to the server\n");
      mulle_spawn_close( handle, NULL);
      return( -1);
   }

   *p_fd = fd;
   return( 0);
}
