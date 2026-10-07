//
//  resp.h
//  miniredis -- RESP2, the wire protocol redis-cli speaks
//
//  Requests are either an array of bulk strings,
//
//     *3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n
//
//  or an inline command, which is what you get from telnet or nc:
//
//     SET k v\r\n
//
//  Replies are one of +simple, -error, :integer, $bulk, *array.
//
#ifndef mredis_resp_h__
#define mredis_resp_h__

#include <mulle-core/mulle-core.h>

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/*
 * A server that reads from an untrusted socket needs all three of these. The
 * bulk length is what an attacker controls most directly: `$1000000000\r\n`
 * must be rejected before it becomes an allocation.
 */
#define MREDIS_MAX_ARGC          128
#define MREDIS_MAX_BULK_LENGTH   (64 * 1024 * 1024)
#define MREDIS_MAX_LINE_LENGTH   (64 * 1024)

/*
 * Accumulates bytes from one connection and cuts whole commands out of them.
 *
 * Command arguments point *into* this buffer (the CRLF after each bulk string
 * is overwritten with a NUL), so they are valid until the next
 * mredis_input_add() -- which is exactly how long a command takes to execute.
 */
struct mredis_input
{
   char         *bytes;
   size_t        length;
   size_t        capacity;
   size_t        offset;     // consumed
   const char   *error;      // set when read_command() returns -1
};


struct mredis_command
{
   size_t        argc;
   char         *argv[ MREDIS_MAX_ARGC];
   size_t        argv_length[ MREDIS_MAX_ARGC];
};


void   mredis_input_init( struct mredis_input *input);
void   mredis_input_done( struct mredis_input *input);

/* 0 on success, -1 if the input could not grow */
int    mredis_input_add( struct mredis_input *input,
                         const void *bytes,
                         size_t length);

/*
 *   1  a command was parsed
 *   0  the buffer holds only part of a command, read more
 *  -1  protocol error, input->error says what (send it, then hang up)
 */
int    mredis_input_read_command( struct mredis_input *input,
                                  struct mredis_command *command);

//
// Replies. They append to a mulle_buffer, which the connection flushes when it
// has finished answering (a pipelined client then gets one write, not one per
// command).
//
void   mredis_reply_simple( struct mulle_buffer *buffer, const char *text);
void   mredis_reply_error( struct mulle_buffer *buffer, const char *text);
void   mredis_reply_integer( struct mulle_buffer *buffer, intptr_t value);
void   mredis_reply_bulk( struct mulle_buffer *buffer,
                          const void *bytes,
                          size_t length);
void   mredis_reply_null_bulk( struct mulle_buffer *buffer);

#ifdef __cplusplus
}
#endif

#endif
