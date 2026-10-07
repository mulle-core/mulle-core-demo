//
//  fake-redis-server.c
//  miniredis -- a stand-in for redis-server, for `--bench --compare --redis` .
//
//  Why it exists: `mredis-stress --bench --compare --redis PATH` spawns a *real*
//  redis-server, learns the port it ended up on, resolves its pid in order to
//  read that process's RSS and CPU, and runs the same workload against both
//  servers. None of that needs redis to be correct -- it needs a server that
//  takes the arguments redis-server takes. This is that server, so the plumbing
//  is covered by the test suite on machines without redis installed.
//
//  Why it is not a shell script any more: a script is a second program. On POSIX
//  it did not matter, because `exec` replaced the shell with mredis and the pid
//  stayed the same; Windows has no `exec`, so a script -- or any wrapper -- would
//  be the process the benchmark started and measured, and the memory and CPU
//  printed next to the second column would describe the wrapper rather than the
//  server. Mapping the arguments and then serving in this process is the only
//  shape that keeps one process on every platform. That the suite no longer
//  needs /bin/sh for it is a side effect.
//
//  It is deliberately strict: anything redis-server would reject as an unknown
//  directive is rejected here too. That is the point -- if the benchmark starts
//  passing an argument redis would refuse, this fails instead of quietly
//  working.
//
//  It is NOT redis, and its numbers are not redis's numbers: they are mredis's,
//  measured twice. Only `--redis $(command -v redis-server)` gives the real
//  comparison.
//
#include <mulle-core/mulle-core.h>

#include <string.h>


// the entry point of mredis.c, named there by -DMREDIS_FIXTURE_MAIN
int   mredis_server_main( int argc, char *argv[]);


int   main( int argc, char *argv[])
{
   char         *forwarded[ 6];
   const char   *port;
   int           i;

   port = NULL;

   for( i = 1; i < argc; i++)
   {
      const char   *option;
      int           takes_value;

      option      = argv[ i];
      takes_value = ! strcmp( option, "--port")
                 || ! strcmp( option, "--bind")
                 || ! strcmp( option, "--save")
                 || ! strcmp( option, "--appendonly")
                 || ! strcmp( option, "--daemonize");

      if( ! takes_value)
      {
         mulle_fprintf( stderr, "fake-redis-server: unknown option %s\n", option);
         return( 1);
      }
      if( i + 1 >= argc)
      {
         mulle_fprintf( stderr, "fake-redis-server: %s wants a value\n", option);
         return( 1);
      }
      if( ! strcmp( option, "--port"))
         port = argv[ i + 1];
      i++;
   }

   if( ! port)
   {
      mulle_fprintf( stderr, "fake-redis-server: --port is required\n");
      return( 1);
   }

   //
   // Loopback whatever address was asked for: the benchmark connects to
   // 127.0.0.1, and what this program is for is the command line redis-server
   // would have been given, not what redis-server would have done with it.
   //
   forwarded[ 0] = argv[ 0];
   forwarded[ 1] = "--port";
   forwarded[ 2] = (char *) port;
   forwarded[ 3] = "--bind";
   forwarded[ 4] = "127.0.0.1";
   forwarded[ 5] = NULL;

   return( mredis_server_main( 5, forwarded));
}
