#include <stdio.h>
#include <string.h>
#include "aspchip.h"

static int bad;
#define CHECK(c) do { if( ! ( c ) ) { printf( "FAIL line %d: %s\n", __LINE__, #c ); bad++; } } while( 0 )

int main( void )
  {
  AspChipHead h, g; unsigned char b[ASP_CHIP_HEAD], c[ASP_CHIP_HEAD]; int v;
  memset( &h, 0, sizeof h );
  h.f.iter = 7; h.f.pass = 123456789012ull; h.f.seq = 99; h.f.peak = 0x123456789abcull; h.f.out_shift = -3;
  h.f.flags = ASP_CHIP_FCLIP; h.f.index = 41; h.f.chips = 100; h.f.frames = 9600; h.f.rate = 48000;
  memset( h.f.session, 0x5a, 32 ); h.f.dcrc = 0xdeadbeef;
  asp_chip_pack( &h, b );
  CHECK( ! memcmp( b, "a5chip\xA5\xA1", 8 ) );
  CHECK( asp_chip_unpack( b, &g, &v ) == 0 && v == 1 );
  CHECK( g.f.iter == 7 && g.f.pass == 123456789012ull && g.f.seq == 99 && g.f.peak == 0x123456789abcull );
  CHECK( g.f.out_shift == -3 && g.f.flags == 1 && g.f.index == 41 && g.f.chips == 100 && g.f.frames == 9600 && g.f.rate == 48000 );
  CHECK( g.f.dcrc == 0xdeadbeef && ! memcmp( g.f.session, h.f.session, 32 ) );
  CHECK( ! memcmp( b + 104, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 23 ) );
  memcpy( c, b, sizeof c ); c[20] ^= 1;                          /* a flipped bit */
  CHECK( asp_chip_unpack( c, &g, &v ) == -3 );
  memcpy( c, b, sizeof c ); c[0] = 'x';                          /* not a chip */
  CHECK( asp_chip_unpack( c, &g, &v ) == -1 && v == 0 );
  memcpy( c, b, sizeof c ); c[7] = 0xA2;                         /* a version from the future */
  CHECK( asp_chip_unpack( c, &g, &v ) == -2 && v == 2 );
  CHECK( asp_crc32( 0, (const unsigned char *)"123456789", 9 ) == 0xCBF43926u );
  printf( bad ? "test_chip: %d FAILED\n" : "test_chip: ok\n", bad );
  return bad != 0;
  }
