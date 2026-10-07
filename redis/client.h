//
//  client.h
//  miniredis -- a small RESP client, shared by the test harness and the
//  benchmark. It exists so that neither of them needs redis-cli, nc or python.
//
#ifndef mredis_client_h__
#define mredis_client_h__

#include <mulle-core/mulle-core.h>

#include <stddef.h>

/*
 * The descriptors here are pipe ends and sockets, and the two platforms read and
 * write them with different names for the same call. Spelling it once keeps the
 * protocol code from having to know which it is on.
 */
#if defined( _WIN32)
# include <fcntl.h>
# include <io.h>
# define mredis_read(fd, buf, n)    _read( fd, buf, n)
# define mredis_write(fd, buf, n)   _write( fd, buf, n)
# define mredis_close(fd)           _close( fd)
#else
# include <sys/types.h>
# include <unistd.h>
# define mredis_read(fd, buf, n)    read( fd, buf, n)
# define mredis_write(fd, buf, n)   write( fd, buf, n)
# define mredis_close(fd)           close( fd)
#endif

/*
 * A process id, spelled once. POSIX has pid_t; Windows has DWORD, and MinGW
 * only happens to define pid_t as well, so a platform whose API says DWORD does
 * not get to lean on a C library's convenience typedef.
 */
#if defined( _WIN32)
typedef unsigned long   mredis_pid_t;
#else
typedef pid_t           mredis_pid_t;
#endif


#if !defined(_WIN32)
# define MREDIS_HAVE_POSIX  1
#endif

/*
 * Reading *another* process's memory and CPU is the one thing here that has no
 * single answer: Linux keeps it in /proc, macOS in libproc, and Windows answers
 * it from the process handle with GetProcessMemoryInfo and GetProcessTimes. A
 * platform with none of them gets no reading rather than a wrong one.
 *
 * A capability and not a platform, on purpose: this is the question the
 * benchmark actually asks -- "can this platform be told about a process it did
 * not write?" -- and it is what the suite gates `--bench` on. MREDIS_HAVE_POSIX
 * is a different question with a different answer, about unix domain sockets,
 * and the two used to be the same macro.
 */
#if defined( __linux__)
# define MREDIS_HAVE_PROC  1
#endif
#if defined( __APPLE__)
# define MREDIS_HAVE_LIBPROC  1
#endif
#if defined( _WIN32)
# define MREDIS_HAVE_WINDOWS_ACCOUNTING  1
#endif

#if defined( MREDIS_HAVE_PROC) \
 || defined( MREDIS_HAVE_LIBPROC) \
 || defined( MREDIS_HAVE_WINDOWS_ACCOUNTING)
# define MREDIS_HAVE_ACCOUNTING  1
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
   MREDIS_REPLY_NONE     = 0,
   MREDIS_REPLY_SIMPLE   = '+',
   MREDIS_REPLY_ERROR    = '-',
   MREDIS_REPLY_INTEGER  = ':',
   MREDIS_REPLY_BULK     = '$',
   MREDIS_REPLY_NULL     = 'n',     // a bulk reply of length -1
   MREDIS_REPLY_ARRAY    = '*',
   MREDIS_REPLY_BROKEN   = '?'
} mredis_reply_type_t;


/*
 * Where a wire's bytes come from. On POSIX a socket *is* a descriptor and the
 * question does not arise, but on Windows a SOCKET is a handle that _read()
 * refuses, so the two need different calls and the wire has to be told which
 * it was given. One field per transport rather than a union of the two, so that
 * neither has to be cast to travel through this struct.
 */
enum mredis_transport
{
   MREDIS_TRANSPORT_DESCRIPTOR = 0,   /* a pipe end, or stdin/stdout */
   MREDIS_TRANSPORT_SOCKET
};


struct mredis_wire
{
   enum mredis_transport   transport;
   int                     fd;          /* MREDIS_TRANSPORT_DESCRIPTOR */
   mulle_socket_t          socket;      /* MREDIS_TRANSPORT_SOCKET */
   char                    buffer[ 4096];
   size_t                  length;
   size_t                  offset;
   unsigned long long      bytes_read;  /* every byte pulled off the wire */
};


struct mredis_reply
{
   mredis_reply_type_t   type;
   long long             integer;      // for INTEGER
   size_t                length;       // for BULK, the value length
   size_t                children;     // for ARRAY, the element count
};


void   mredis_wire_init( struct mredis_wire *wire, int fd);
void   mredis_wire_init_socket( struct mredis_wire *wire, mulle_socket_t socket);
int    mredis_wire_getc( struct mredis_wire *wire);
int    mredis_wire_read( struct mredis_wire *wire, char *dst, size_t length);
int    mredis_wire_read_line( struct mredis_wire *wire, char *dst, size_t size);

/*
 * Reads one reply. A BULK reply is copied into `value`, which must be big
 * enough; anything larger is reported as BROKEN rather than truncated, so this
 * is for the benchmark's small values, not a general client.
 */
int    mredis_wire_read_reply( struct mredis_wire *wire,
                               struct mredis_reply *reply,
                               char *value,
                               size_t value_size);

/* Encoding. Text form is `SET k "a b c"`; the argv form is what the benchmark
 * uses, because it already has lengths. */
void   mredis_encode_command( const char *line, struct mulle_buffer *buffer);
void   mredis_encode_argv( struct mulle_buffer *buffer,
                           size_t argc,
                           const char *const *argv,
                           const size_t *argv_length);

int    mredis_write_all( int fd, const void *bytes, size_t length);

/*
 * Spawning is the one thing here that is portable, so these live outside the
 * sockets guard: a stdio server needs no network at all. Both return 0 and
 * leave the child in `handle`, which the caller terminates and closes -- and
 * whose `pid` is what the memory + CPU accounting asks about.
 */
int    mredis_spawn_stdio( const char *server, struct mulle_spawn_handle *handle);
int    mredis_spawn_server( const char *server,
                            unsigned int *p_port,
                            struct mulle_spawn_handle *handle);

/*
 * Sockets go through mulle-socket, so nothing below is POSIX-only any more.
 * The handles are mulle_socket_t -- a SOCKET on Windows, which is a handle and
 * not a CRT descriptor, which is why they are not ints and why MULLE_SOCKET_INVALID
 * is the failure to test for rather than a negative number.
 */
int    mredis_spawn_tcp( const char *server,
                        mulle_socket_t *p_fd,
                        struct mulle_spawn_handle *handle);

mulle_socket_t   mredis_connect( const char *host, unsigned int port);

/*
 * A TCP port nobody is using, for a server we are about to start. Racy by
 * nature -- the port is free when we check and could be taken a moment later --
 * which is why the caller retries rather than trusting it.
 */
int    mredis_find_free_port( unsigned int *p_port);

/*
 * Starts any Redis-compatible server on a TCP port we picked, for benching a
 * real `redis-server` the way we bench `mredis`. Waits for the port to answer,
 * and returns the pid so the caller can stop it and read its memory.
 */
int    mredis_spawn_tcp_port( const char *server,
                              const char *const *extra_argv,
                              unsigned int *p_port,
                              struct mulle_spawn_handle *handle);

/*
 * Unix domain sockets. mulle-socket offers them on POSIX only, so there this is
 * mulle_socket_connect_unix and on Windows it is a refusal rather than an
 * attempt -- the option parser declines a `/path` spec as well. Declared
 * everywhere because the caller has no guard of its own to hide behind.
 */
mulle_socket_t   mredis_connect_unix( const char *path);

/*
 * Whose process is listening on this port? A server we did not start can still
 * be asked how much memory it is using, and how much CPU it burned. Linux
 * resolves it through /proc/net/tcp -> socket inode -> /proc/<pid>/fd; macOS
 * walks every process's descriptor table with libproc; Windows reads the owner
 * straight out of its TCP table. -1 where none of that is available, or where
 * the process is not this user's to inspect.
 *
 * Defined on every platform: one that cannot answer returns -1, which is the
 * same answer the permissions case gives, and both leave the accounting
 * reporting n/a instead of a zero that reads like a measurement.
 */
long   mredis_pid_for_port( unsigned int port);

#ifdef __cplusplus
}
#endif

#endif
