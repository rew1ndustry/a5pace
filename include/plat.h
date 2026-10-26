/* the few things the core needs from the platform. everything else is plain C99 + stdio.
   shells and edges (audio devices, sockets, ttys) live outside the core. */
#ifndef ASP_PLAT_H
#define ASP_PLAT_H
#include <stdio.h>
#include <stdint.h>

#if defined(_WIN32)
# define ASP_FSEEK(f,o,w) _fseeki64( f, (long long)(o), w )
# define ASP_FTELL(f)     _ftelli64( f )
#else
# define ASP_FSEEK(f,o,w) fseeko( f, (off_t)(o), w )
# define ASP_FTELL(f)     ftello( f )
#endif

/* on disk and in the pipe everything is little endian, whatever the host (aix is big endian) */
static inline uint32_t asp_ld32( const unsigned char *p )
  { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t asp_ld64( const unsigned char *p )
  { return (uint64_t)asp_ld32( p ) | (uint64_t)asp_ld32( p + 4 ) << 32; }
static inline void asp_st32( unsigned char *p, uint32_t v )
  { p[0] = (unsigned char)v; p[1] = (unsigned char)( v >> 8 ); p[2] = (unsigned char)( v >> 16 ); p[3] = (unsigned char)( v >> 24 ); }
static inline void asp_st64( unsigned char *p, uint64_t v )
  { asp_st32( p, (uint32_t)v ); asp_st32( p + 4, (uint32_t)( v >> 32 ) ); }
#endif
