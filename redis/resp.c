//
//  resp.c
//  miniredis
//

#include "resp.h"

#include <stdint.h>
#include <string.h>


// A command may legitimately be one 64MB bulk plus its header, so the input
// buffer has to be allowed to grow that far -- and no further.
#define MREDIS_MAX_INPUT_LENGTH  (MREDIS_MAX_BULK_LENGTH + MREDIS_MAX_LINE_LENGTH)


static char   *mredis_find_crlf( char *p, char *end)
{
   while( p + 1 < end)
   {
      if( p[ 0] == '\r' && p[ 1] == '\n')
         return( p);
      p++;
   }
   return( NULL);
}


// Strict: digits only, no sign, no leading '+', no overflow.
static int   mredis_parse_number( char *p, char *end, int64_t *p_value)
{
   int64_t   value;

   value = 0;
   if( p == end)
      return( -1);

   for( ; p < end; p++)
   {
      if( *p < '0' || *p > '9')
         return( -1);
      if( value > (INT64_MAX - (*p - '0')) / 10)
         return( -1);
      value = value * 10 + (*p - '0');
   }

   *p_value = value;
   return( 0);
}


static void   mredis_add_byte( struct mulle_buffer *buffer, char c)
{
   mulle_buffer_add_bytes( buffer, &c, 1);
}


static void   mredis_add_size( struct mulle_buffer *buffer, uint64_t value)
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

   mulle_buffer_add_bytes( buffer, &tmp[ i], sizeof( tmp) - i);
}


void   mredis_input_init( struct mredis_input *input)
{
   memset( input, 0, sizeof( struct mredis_input));
}


void   mredis_input_done( struct mredis_input *input)
{
   mulle_free( input->bytes);
   memset( input, 0, sizeof( struct mredis_input));
}


int   mredis_input_add( struct mredis_input *input, const void *bytes, size_t length)
{
   char     *new_bytes;
   size_t    capacity;
   size_t    remaining;
   size_t    needed;

   /*
    * Drop what has been consumed. This is also what invalidates the argument
    * pointers of the previous command, so callers must finish executing a
    * command before reading more.
    */
   if( input->offset)
   {
      remaining = input->length - input->offset;
      if( remaining)
         memmove( input->bytes, input->bytes + input->offset, remaining);
      input->length = remaining;
      input->offset = 0;
   }

   needed = input->length + length;
   if( needed > MREDIS_MAX_INPUT_LENGTH)
      return( -1);

   if( needed > input->capacity)
   {
      capacity = input->capacity ? input->capacity : 4096;
      while( capacity < needed)
         capacity *= 2;
      if( capacity > MREDIS_MAX_INPUT_LENGTH)
         capacity = MREDIS_MAX_INPUT_LENGTH;

      new_bytes = mulle_realloc( input->bytes, capacity);
      if( ! new_bytes)
         return( -1);

      input->bytes    = new_bytes;
      input->capacity = capacity;
   }

   if( length)
      memcpy( input->bytes + input->length, bytes, length);
   input->length += length;
   return( 0);
}


/*
 * Inline command: everything up to the newline, split on whitespace.
 * No quoting -- RESP inline is a convenience for telnet, not a shell.
 */
static int   mredis_input_read_inline( struct mredis_input *input,
                                       struct mredis_command *command)
{
   char     *p;
   char     *end;
   char     *start;
   char     *line;
   size_t    argc;
   size_t    i;

   p   = input->bytes + input->offset;
   end = input->bytes + input->length;

   line = memchr( p, '\n', (size_t) (end - p));
   if( ! line)
   {
      if( (size_t) (end - p) > MREDIS_MAX_LINE_LENGTH)
      {
         input->error = "too big inline request";
         return( -1);
      }
      return( 0);
   }

   end = line;
   if( end > p && end[ -1] == '\r')
      end--;

   start = p;
   argc  = 0;
   while( p < end)
   {
      while( p < end && (*p == ' ' || *p == '\t'))
         p++;
      if( p == end)
         break;
      if( argc == MREDIS_MAX_ARGC)
      {
         input->error = "too many arguments";
         return( -1);
      }

      command->argv[ argc] = p;
      while( p < end && *p != ' ' && *p != '\t')
         p++;
      command->argv_length[ argc] = (size_t) (p - command->argv[ argc]);
      argc++;
   }

   // Now that the line is known to be complete, the token separators can be
   // overwritten in place. The byte each token ends on is a space or the CR/LF
   // itself, so this stays inside the buffer.
   for( i = 0; i < argc; i++)
      command->argv[ i][ command->argv_length[ i]] = 0;

   command->argc = argc;
   input->offset = (size_t) ((line + 1) - input->bytes);
   (void) start;
   return( 1);
}


int   mredis_input_read_command( struct mredis_input *input,
                                 struct mredis_command *command)
{
   char     *p;
   char     *end;
   char     *line;
   int64_t   count;
   int64_t   number;
   size_t    argc;
   size_t    i;

   command->argc = 0;
   input->error  = NULL;

   p   = input->bytes + input->offset;
   end = input->bytes + input->length;
   if( p == end)
      return( 0);

   if( *p != '*')
      return( mredis_input_read_inline( input, command));

   line = mredis_find_crlf( p, end);
   if( ! line)
      return( 0);

   if( mredis_parse_number( p + 1, line, &count))
   {
      input->error = "invalid multibulk length";
      return( -1);
   }

   p = line + 2;
   if( count <= 0)
   {
      // `*0\r\n` and `*-1\r\n` are legal and mean "no command".
      input->offset = (size_t) (p - input->bytes);
      return( 1);
   }

   if( count > MREDIS_MAX_ARGC)
   {
      input->error = "too many arguments";
      return( -1);
   }

   /*
    * Pass one validates and locates, pass two terminates in place. The two
    * passes matter: terminating an argument writes over the CRLF that follows
    * it, so a command that turns out to be incomplete must not have been
    * touched, or re-parsing it after the rest arrives would fail.
    */
   argc = 0;
   for( i = 0; i < (size_t) count; i++)
   {
      /*
       * The end of the buffer in the middle of a command is not an error: the
       * rest of the batch simply has not arrived yet. This is the normal case
       * for a pipelining client whose batch is larger than one read (16 KB),
       * and getting it wrong turned a valid batch into "expected '$', got
       * something else" about 170 commands in. This test needs to stay ahead of
       * the `$` check, because `end` is a byte we are allowed to be short of.
       */
      if( p >= end)
         return( 0);

      if( *p != '$')
      {
         input->error = "expected '$', got something else";
         return( -1);
      }

      line = mredis_find_crlf( p, end);
      if( ! line)
         return( 0);

      if( mredis_parse_number( p + 1, line, &number) ||
          number < 0 ||
          number > MREDIS_MAX_BULK_LENGTH)
      {
         input->error = "invalid bulk length";
         return( -1);
      }

      p = line + 2;
      if( (size_t) (end - p) < (size_t) number + 2)
         return( 0);

      if( p[ number] != '\r' || p[ number + 1] != '\n')
      {
         input->error = "unbalanced bulk string";
         return( -1);
      }

      command->argv[ i]        = p;
      command->argv_length[ i] = (size_t) number;
      p                       += number + 2;
      argc++;
   }

   for( i = 0; i < argc; i++)
      command->argv[ i][ command->argv_length[ i]] = 0;

   command->argc = argc;
   input->offset = (size_t) (p - input->bytes);
   return( 1);
}


//
// replies
//
void   mredis_reply_simple( struct mulle_buffer *buffer, const char *text)
{
   mredis_add_byte( buffer, '+');
   mulle_buffer_add_bytes( buffer, text, strlen( text));
   mulle_buffer_add_bytes( buffer, "\r\n", 2);
}


/*
 * Error text is sanitized: a CR or LF inside it would end the reply early and
 * let a value the client sent become protocol.
 */
void   mredis_reply_error( struct mulle_buffer *buffer, const char *text)
{
   size_t   i;
   size_t   length;

   mredis_add_byte( buffer, '-');

   length = strlen( text);
   for( i = 0; i < length; i++)
   {
      char   c;

      c = text[ i];
      mredis_add_byte( buffer, (c == '\r' || c == '\n') ? ' ' : c);
   }
   mulle_buffer_add_bytes( buffer, "\r\n", 2);
}


void   mredis_reply_integer( struct mulle_buffer *buffer, intptr_t value)
{
   uint64_t   magnitude;

   mredis_add_byte( buffer, ':');
   if( value < 0)
   {
      mredis_add_byte( buffer, '-');
      magnitude = (uint64_t) (-(value + 1)) + 1;
   }
   else
      magnitude = (uint64_t) value;

   mredis_add_size( buffer, magnitude);
   mulle_buffer_add_bytes( buffer, "\r\n", 2);
}


void   mredis_reply_bulk( struct mulle_buffer *buffer,
                          const void *bytes,
                          size_t length)
{
   mredis_add_byte( buffer, '$');
   mredis_add_size( buffer, length);
   mulle_buffer_add_bytes( buffer, "\r\n", 2);
   if( length)
      mulle_buffer_add_bytes( buffer, bytes, length);
   mulle_buffer_add_bytes( buffer, "\r\n", 2);
}


void   mredis_reply_null_bulk( struct mulle_buffer *buffer)
{
   // RESP2 spells a missing value as a -1 length bulk, not as a null type.
   mulle_buffer_add_bytes( buffer, "$-1\r\n", 5);
}
