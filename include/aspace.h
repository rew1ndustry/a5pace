/* aspace.h: the one header for a host that embeds the core (a phone app, a plugin, a test, a5pace itself).
   link libaspace.a (make lib) with libFLAC and sqlite. the library has no main, no sockets, no audio device, no clock and
   no threads of its own but the keeper (keeper.h). a host does three things:

     1. open:   AspStore *s = asp_open( dir, 0, err );  AspTank *t = asp_tank_open( dir/loop.d64, err, n );
                AspCoreCfg cfg; asp_core_defaults( &cfg ); AspCore *c = asp_core_open( s, t, &cfg, err, n );
                (asp_create / asp_tank_create make a new space; asp_tank_bind ties the tank to the session)
     2. tick:   once per ASP_TICK (9600) frames of 48 kHz stereo int32, from the audio thread's ring buffers:
                asp_core_tick( c, in, out );   in = what was recorded, out = what to play; both interleaved L R
     3. close:  asp_core_drain( c ) until asp_core_moves( c ) is 0 (queued moves must finish), then asp_core_close( c ),
                asp_tank_close( t ), asp_close( s ).

   the audio callback must never call asp_core_tick directly if it can block (a tick does file work): a worker thread ticks,
   the callback only moves frames in and out of two ring buffers (examples/embed.c shows the shape; src/a5sdl.c is a host with
   rings, as a program). control (mix, safe, sync, undo ...) is the asp_core_* calls in aspcore.h, from the thread that ticks.
   a5pace's control port (ctlsrv.c) is one host's way of reaching them, not part of the library. */
#ifndef ASPACE_H
#define ASPACE_H
#include "aspstore.h"
#include "asptank.h"
#include "aspengine.h"
#include "aspcore.h"
#include "asppeaks.h"
#include "settings.h"
#endif
