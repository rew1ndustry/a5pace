#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "settings.h"

int asp_config_dir( char *buf, size_t n )
  {
  const char *e = getenv( "A5PACE_CONFIG" ), *x = getenv( "XDG_CONFIG_HOME" ), *h = getenv( "HOME" );
  if( e && *e ) snprintf( buf, n, "%s", e );
  else if( x && *x ) snprintf( buf, n, "%s/a5pace", x );
  else if( h && *h ) snprintf( buf, n, "%s/.config/a5pace", h );
  else return -1;
  return 0;
  }

int asp_profile_path( const char *name, char *buf, size_t n )
  {
  char d[900];
  if( ! name || ! *name || strlen( name ) > 60 ) return -1;
  for( const char *p = name; *p; p++ ) if( ! isalnum( (unsigned char)*p ) && *p != '-' && *p != '_' && *p != '.' ) return -1;
  if( name[0] == '.' ) return -1;
  if( asp_config_dir( d, sizeof d ) ) return -1;
  snprintf( buf, n, "%s/profiles/%s", d, name );
  return 0;
  }

static void put_kv( char *out, size_t n, const char *k, const char *v )
  {
  size_t l = strlen( out );
  snprintf( out + l, n - l, "%s%s=%s", l ? ";" : "", k, v );
  }

int asp_settings_file( const char *path, char *out, size_t n )
  {
  FILE *f = fopen( path, "r" );
  char line[400];
  if( ! f ) return -1;
  out[0] = 0;
  while( fgets( line, sizeof line, f ) )
    {
    char *p = line, *k, *v;
    while( isspace( (unsigned char)*p ) ) p++;
    if( ! *p || *p == '#' ) continue;
    k = p;
    while( *p && *p != '=' && ! isspace( (unsigned char)*p ) ) p++;
    if( *p ) *p++ = 0;
    while( *p == '=' || isspace( (unsigned char)*p ) ) p++;
    v = p;
    for( p = v + strlen( v ); p > v && isspace( (unsigned char)p[-1] ); ) *--p = 0;
    for( p = v; *p; p++ ) if( *p == ';' || *p == '"' || *p == '\\' ) *p = ' ';      /* the stored form is ; separated and json safe */
    if( *k && *v ) put_kv( out, n, k, v );
    }
  fclose( f );
  return 0;
  }

int asp_settings_get( const char *text, const char *key, char *val, size_t n )
  {
  size_t kl = strlen( key );
  const char *p = text;
  while( p && *p )
    {
    const char *e = strchr( p, ';' );
    size_t len = e ? (size_t)( e - p ) : strlen( p );
    if( len > kl && ! strncmp( p, key, kl ) && p[kl] == '=' )
      { size_t vl = len - kl - 1; if( vl >= n ) vl = n - 1; memcpy( val, p + kl + 1, vl ); val[vl] = 0; return 0; }
    p = e ? e + 1 : NULL;
    }
  return -1;
  }

void asp_settings_merge( char *base, size_t n, const char *over )
  {
  char out[1100] = "", k[64], v[200];
  const char *p;
  /* base keys not overridden, then everything in over */
  for( p = base; p && *p; )
    {
    const char *e = strchr( p, ';' ), *q = strchr( p, '=' );
    size_t len = e ? (size_t)( e - p ) : strlen( p );
    if( q && (size_t)( q - p ) < len && (size_t)( q - p ) < sizeof k )
      {
      memcpy( k, p, (size_t)( q - p ) ); k[q - p] = 0;
      if( asp_settings_get( over, k, v, sizeof v ) )
        { size_t l = strlen( out ); snprintf( out + l, sizeof out - l, "%s%.*s", l ? ";" : "", (int)len, p ); }
      }
    p = e ? e + 1 : NULL;
    }
  for( p = over; p && *p; )
    {
    const char *e = strchr( p, ';' );
    size_t len = e ? (size_t)( e - p ) : strlen( p );
    if( len ) { size_t l = strlen( out ); snprintf( out + l, sizeof out - l, "%s%.*s", l ? ";" : "", (int)len, p ); }
    p = e ? e + 1 : NULL;
    }
  snprintf( base, n, "%s", out );
  }

void asp_settings_cfg( AspCoreCfg *c, const char *t )
  {
  char v[100];
  if( ! asp_settings_get( t, "gate", v, sizeof v ) ) c->gate_db = atof( v );
  if( ! asp_settings_get( t, "ring", v, sizeof v ) ) c->ring_db = atof( v );
  if( ! asp_settings_get( t, "lowcut", v, sizeof v ) ) c->lowcut = atof( v );
  if( ! asp_settings_get( t, "strict", v, sizeof v ) ) c->strict = atoi( v );
  if( ! asp_settings_get( t, "mono", v, sizeof v ) ) c->mono_detect = atoi( v );
  if( ! asp_settings_get( t, "sync_max", v, sizeof v ) ) c->sync.max_db = atof( v );
  if( ! asp_settings_get( t, "sync_start", v, sizeof v ) ) c->sync.start_db = atof( v );
  if( ! asp_settings_get( t, "sync_target", v, sizeof v ) ) c->sync.target_db = atof( v );
  if( ! asp_settings_get( t, "automix", v, sizeof v ) )
    {
    c->automix.mode = ! strncmp( v, "overlap", 7 ) ? ASP_AUTOMIX_OVERLAP : ! strncmp( v, "swap", 4 ) ? ASP_AUTOMIX_SWAP : ASP_AUTOMIX_OFF;
    const char *sp = strchr( v, ' ' );
    c->automix.n = sp ? atoi( sp + 1 ) : 4;
    c->automix.each = sp && strstr( sp, "each" ) != NULL;
    }
  }

static int cmp( const void *a, const void *b ) { return strcmp( *(char *const *)a, *(char *const *)b ); }

int asp_profile_list( AspProfileCb cb, void *user )
  {
  char d[900], path[1100], *names[200];
  int n = 0;
  if( asp_config_dir( d, sizeof d ) ) return 0;
  snprintf( path, sizeof path, "%s/profiles", d );
  DIR *dir = opendir( path );
  if( ! dir ) return 0;
  struct dirent *e;
  while( ( e = readdir( dir ) ) && n < 200 )
    if( e->d_name[0] != '.' ) names[n++] = strdup( e->d_name );
  closedir( dir );
  qsort( names, (size_t)n, sizeof *names, cmp );
  for( int i = 0; i < n; i++ )
    {
    char note[160] = "", line[300];
    snprintf( path, sizeof path, "%s/profiles/%s", d, names[i] );
    FILE *f = fopen( path, "r" );
    if( f ) { while( fgets( line, sizeof line, f ) ) if( line[0] == '#' ) { char *p = line + 1; while( *p == ' ' ) p++; p[strcspn( p, "\r\n" )] = 0; snprintf( note, sizeof note, "%s", p ); break; } fclose( f ); }
    cb( names[i], note, user );
    free( names[i] );
    }
  return n;
  }
