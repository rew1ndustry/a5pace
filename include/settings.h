/* settings: key=value lists that carry a setup. one text form everywhere: "gate=18;ring=7.5;lowcut=190".
   files (defaults, profiles) are one "key value" or "key=value" per line, # comments; the first comment line of a
   profile is its description in the list. profiles are made outside a session (by calibration) and chosen from a list.
     <config>/defaults             inherited by every new session
     <config>/profiles/NAME        one setup: a gate and sync setting for an instrument, a room, a signal chain
   <config> is $A5PACE_CONFIG, else $XDG_CONFIG_HOME/a5pace, else ~/.config/a5pace.
   keys: gate ring lowcut strict mono sync_max sync_start sync_target automix  (see docs/PROFILES.md) */
#ifndef ASPSETTINGS_H
#define ASPSETTINGS_H
#include <stddef.h>
#include "aspcore.h"

int  asp_config_dir( char *buf, size_t n );
int  asp_profile_path( const char *name, char *buf, size_t n );          /* 0 if the name is a plain name */
int  asp_settings_file( const char *path, char *out, size_t n );         /* a file as "k=v;k=v"; -1 if unreadable */
void asp_settings_merge( char *base, size_t n, const char *over );       /* over wins, key by key */
int  asp_settings_get( const char *text, const char *key, char *val, size_t n );
void asp_settings_cfg( AspCoreCfg *cfg, const char *text );              /* the open-time keys into a core config */
typedef void (*AspProfileCb)( const char *name, const char *note, void *user );
int  asp_profile_list( AspProfileCb cb, void *user );                    /* number of profiles */
#endif
