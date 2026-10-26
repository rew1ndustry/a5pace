/* aspflac: .d32 (raw 32 bit pcm) <-> flac, using libFLAC through flacio only.
     aspflac enc IN.d32 OUT.flac [channels [rate]]
     aspflac dec IN.flac OUT.d32|-
     aspflac md5 IN.flac        md5 of the decoded pcm bytes, and the frame count */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flacio.h"

int main( int argc, char **argv )
  {
  if( argc >= 4 && ! strcmp( argv[1], "enc" ) )
    return asp_flac_encode_file( argv[2], argv[3], argc > 4 ? atoi( argv[4] ) : 2,
                                 argc > 5 ? atoi( argv[5] ) : 48000 ) ? 1 : 0;
  if( argc == 4 && ! strcmp( argv[1], "dec" ) ) return asp_flac_decode_file( argv[2], argv[3] ) ? 1 : 0;
  if( argc == 3 && ! strcmp( argv[1], "md5" ) )
    {
    unsigned char m[16]; uint64_t n;
    if( asp_flac_info( argv[2], m, &n, NULL, NULL, NULL ) ) return 1;
    for( int i = 0; i < 16; i++ ) printf( "%02x", m[i] );
    printf( " %llu\n", (unsigned long long)n );
    return 0;
    }
  fprintf( stderr, "usage: aspflac enc IN.d32 OUT.flac [ch [rate]] | dec IN.flac OUT.d32|- | md5 IN.flac\n" );
  return 2;
  }
