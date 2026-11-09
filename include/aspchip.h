/* aspchip: the header of one chip of the loop (chip model, see CLAUDE.md "Moves rule").
   A chip is one tick of the loop: a 128 byte header then its frames of two s64 (L then R), all little endian.
   The tank is chips back to back: no tank header, no loop file header. Every chip is complete in itself.

   In memory the header is a union, so the struct can grow into the pad without changing the 128 bytes.
   On disk it is packed field by field with asp_st32/asp_st64 (endian safe), never by copying the struct.

     0   magic[8]   "a5chip" 0xA5 (0xA0 | version)   the low nibble of the last byte is the structure version (1..15)
     8   u64 iter          iteration of the loop this chip belongs to
     16  u64 pass          pass (lap) that last wrote this chip
     24  u64 seq           log point this chip reflects
     32  u64 peak          loop peak to peak, as of this write
     40  i32 out_shift     output gain: how far the output was brought down to fit (asp_mix_out's shift)
     44  u32 flags         bit 0 = this chip was cut by a clip (gain sloped over it)
     48  u32 index         chip number, 0..chips-1
     52  u32 chips         chips in the loop
     56  u32 frames        frames in this chip (ASP_TICK, the last may be fewer)
     60  u32 rate
     64  u8  session[32]   sha-256 of the store's session, zero if not bound
     96  u32 dcrc          crc32 of the frames as written (the 16 * frames bytes after the header)
     100 u32 hcrc          crc32 of this header with hcrc zero
     104 24 bytes reserved, zero

   The iterations on disk must either all match or fall by exactly 1 at one edge (the restart point). Anything else
   is a corrupt loop: a5rebuild, outside the core. */
#ifndef ASPCHIP_H
#define ASPCHIP_H
#include <stdint.h>
#include <stddef.h>

#define ASP_CHIP_HEAD    128
#define ASP_CHIP_VERSION 1
#define ASP_CHIP_FCLIP   1u

typedef union
  {
  struct
    {
    unsigned char magic[8];
    uint64_t iter, pass, seq, peak;
    int32_t  out_shift;
    uint32_t flags, index, chips, frames, rate;
    unsigned char session[32];
    uint32_t dcrc, hcrc;
    } f;
  unsigned char pad[ASP_CHIP_HEAD];
  } AspChipHead;

/* the magic for a structure version (1..15) */
void asp_chip_magic( unsigned char m[8], int version );

/* crc32 (zlib's), running: start from 0 */
uint32_t asp_crc32( uint32_t crc, const unsigned char *p, size_t n );

/* head -> 128 bytes. sets the magic (current version) and hcrc; dcrc is the caller's (asp_crc32 of the frame bytes) */
void asp_chip_pack( const AspChipHead *h, unsigned char out[ASP_CHIP_HEAD] );

/* 128 bytes -> head. 0 if good; -1 not a chip (magic), -2 a structure version this code does not know, -3 bad hcrc.
   *version, if given, gets the version nibble whenever the magic matched */
int  asp_chip_unpack( const unsigned char in[ASP_CHIP_HEAD], AspChipHead *h, int *version );
#endif
