#include <stddef.h>
#include <string.h>
#include "aspchip.h"
#include "plat.h"

typedef char chip_head_is_128[ sizeof( AspChipHead ) == ASP_CHIP_HEAD ? 1 : -1 ];

void asp_chip_magic( unsigned char m[8], int version )
  {
  memcpy( m, "a5chip", 6 );
  m[6] = 0xA5;
  m[7] = (unsigned char)( 0xA0 | ( version & 15 ) );
  }

uint32_t asp_crc32( uint32_t crc, const unsigned char *p, size_t n )
  {
  static uint32_t tab[256];
  if( ! tab[1] )
    for( uint32_t i = 0; i < 256; i++ )
      { uint32_t c = i; for( int k = 0; k < 8; k++ ) c = c & 1 ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1; tab[i] = c; }
  crc = ~crc;
  while( n-- ) crc = tab[ ( crc ^ *p++ ) & 255 ] ^ ( crc >> 8 );
  return ~crc;
  }

void asp_chip_pack( const AspChipHead *h, unsigned char o[ASP_CHIP_HEAD] )
  {
  memset( o, 0, ASP_CHIP_HEAD );
  asp_chip_magic( o, ASP_CHIP_VERSION );
  asp_st64( o + 8, h->f.iter ); asp_st64( o + 16, h->f.pass ); asp_st64( o + 24, h->f.seq ); asp_st64( o + 32, h->f.peak );
  asp_st32( o + 40, (uint32_t)h->f.out_shift ); asp_st32( o + 44, h->f.flags );
  asp_st32( o + 48, h->f.index ); asp_st32( o + 52, h->f.chips ); asp_st32( o + 56, h->f.frames ); asp_st32( o + 60, h->f.rate );
  memcpy( o + 64, h->f.session, 32 );
  asp_st32( o + 96, h->f.dcrc );
  asp_st32( o + 100, asp_crc32( 0, o, ASP_CHIP_HEAD ) );   /* hcrc was zero while summed */
  }

int asp_chip_unpack( const unsigned char in[ASP_CHIP_HEAD], AspChipHead *h, int *version )
  {
  unsigned char b[ASP_CHIP_HEAD], m[8];
  if( version ) *version = 0;
  if( memcmp( in, "a5chip\xA5", 7 ) || ( in[7] & 0xF0 ) != 0xA0 ) return -1;
  if( version ) *version = in[7] & 15;
  asp_chip_magic( m, ASP_CHIP_VERSION );
  if( memcmp( in, m, 8 ) ) return -2;
  memcpy( b, in, ASP_CHIP_HEAD ); memset( b + 100, 0, 4 );
  if( asp_crc32( 0, b, ASP_CHIP_HEAD ) != asp_ld32( in + 100 ) ) return -3;
  memset( h, 0, sizeof *h );
  memcpy( h->f.magic, in, 8 );
  h->f.iter = asp_ld64( in + 8 ); h->f.pass = asp_ld64( in + 16 ); h->f.seq = asp_ld64( in + 24 ); h->f.peak = asp_ld64( in + 32 );
  h->f.out_shift = (int32_t)asp_ld32( in + 40 ); h->f.flags = asp_ld32( in + 44 );
  h->f.index = asp_ld32( in + 48 ); h->f.chips = asp_ld32( in + 52 ); h->f.frames = asp_ld32( in + 56 ); h->f.rate = asp_ld32( in + 60 );
  memcpy( h->f.session, in + 64, 32 );
  h->f.dcrc = asp_ld32( in + 96 ); h->f.hcrc = asp_ld32( in + 100 );
  return 0;
  }
