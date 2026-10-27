#!/usr/bin/env python3
"""print asp4's sync routines (dith/sine, audInfo, audCross, putSync..findSync) as a standalone reference"""
import sys, os
S = open(sys.argv[1]).read()
def seg(a, b):
    i = S.index(a); j = S.index(b, i); return S[i:j]
print('''#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef int32_t s4; typedef int64_t s8; typedef uint64_t u8; typedef uint32_t u4;
#define HZ 48000
#define PADSAM 9600
static void dbug( const char *f, ... ) { (void)f; }
static int my_open( char *p, int m ) { (void)p; (void)m; return 0; }
static int my_write( int fd, char *p, int n ) { (void)fd; (void)p; return n; }
static int my_close( int fd ) { (void)fd; return 0; }
#define DB_ dbug(
#define _UG )
static u4 my_noise = 250;''')
print(seg("static s4 _dith", "static s4 audShift"))
print(seg("typedef struct\n  {\n  u4 rms;", "// seek given channel only, reject noise"))
print(seg("static s4 *audCross", "/* static s4 *audSync"))
print(seg("static int sync_range", "static void _test_sync"))
print('''void ref_put( char *d, int sec ) { putSync( d, 384000, sec ); }
s8 ref_find( char *d, int sec ) { return findSync( d, 384000, sec, "" ); }''')
