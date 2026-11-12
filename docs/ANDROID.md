# Android: plan (2026-11-12, nothing built yet)

The core is a library (`make lib`, include/aspace.h), so a phone host is: an app with a native library (libaspace.a + libFLAC + sqlite built
with the NDK), a worker thread that ticks the core, and two ring buffers filled and emptied by AAudio callbacks (the C API in the NDK, no
extra download; Oboe wraps it for older phones). The space (session dir, take files, loop file) lives in the app's own storage.
Steps, each testable on its own:
1. **Tools** (needs a download of about 1.5 GB, and a phone with developer mode and USB debugging on, authorised once on its screen):
   platform-tools (`adb`) and the NDK, into `~/.local/android/`. Nothing system-wide.
2. **Cross-build the library and `bin/embed`** for arm64 with the NDK's clang (libFLAC through its CMake, sqlite from third_party). Push `embed` with `adb push`
   and run it with `adb shell`: this needs no app and no audio, and proves the core, the store and the tank run on the phone (a Linux kernel,
   so plain executables work from `/data/local/tmp`). Only then:
3. **An audio host**: a small native program or app using AAudio at 48 kHz stereo, 32-bit (or float and convert), `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`,
   exclusive sharing, input and output on the same USB device when one is attached. The callbacks only move frames to and from the rings.
4. **A display**: a5web in the phone's browser, if the app also runs the small http server (a5web is C and already loopback-only), or a native view later.

## The UR22C over USB-C
Probably yes, with caveats, none tested here:
- The UR22C is a USB audio class device (it has a class-compliant mode, which is how iPads use it), and Android has a class-driver for USB audio (UAC1 and UAC2), so
  the phone should list it as an audio device with 2 in and 2 out. AAudio can be told which device (`setDeviceId`) for input and output.
- **Power**: the UR22C wants more than many phones will give on the bus; it has a second USB-C port for power from a charger for exactly this case. Expect to need it, and
  a powered hub or a splitter that charges the phone as well if you want it to keep running.
- **Latency and format**: USB audio on Android works at 48 kHz; round trip is typically well over 40 ms through the USB stack, which the sync hunt measures anyway. Whether it hands us
  32-bit samples or converts from 24-bit is device-dependent; the host would convert to s32 either way.
- **The loopback** for the sync hunt: on a5card the shell switches the UR22C's own loopback with a script (`ur22c loopback on|off`); the phone has no such tool, so the first
  version would use the acoustic or cable loopback, or the declared latency (`declared_latency`), as the edge contract allows.
- **Samsung specifics**: some Samsung phones route USB audio only to the 'USB audio' output and need the input chosen explicitly; try it, then record what happened here.

## Done 2026-11-12: first contact
- Phone: Samsung Galaxy A14 5G (SM-A146W), Android 15, arm64-v8a, USB debugging authorised. Tools in `~/.local/android/` (platform-tools, NDK r27c).
- `tools/a5droid-build.sh` cross-builds libaspace.a and `embed` with no source change. `adb push` to `/data/local/tmp` and run in `adb shell`: a tone recorded, the take
  committed on close, and on a second run played back from the loop. So the core, the store (sqlite), flac and the tank all run on the phone as they are.
- Audio the phone reports (`dumpsys media.audio_policy`): built-in mic (bottom), back mic, a "2 Mic" dual-mic input, USB device / USB headset in and out; the built-in
  path is 16-bit PCM at up to 48 kHz (the HAL's format), USB in lists up to 192 kHz stereo. Built-in capture is also subject to Samsung's voice processing unless the host
  asks for the "unprocessed" / "voice performance" input preset; mic quality has not been measured (needs a recording test).
- Next: an audio host (AAudio, rings, worker thread) as a small NDK program, then try the UR22C (docs above), then AMidi for its loopback SysEx.

## a5droid: the smoke-test host (2026-11-12)
`src/a5droid.c` (built by `tools/a5droid-build.sh`): AAudio at 48 kHz stereo float, callbacks through two lock-free rings of 8 ticks, a worker thread ticks the core, drops and starves
are told to the core. The round trip is declared (`--latency`, default 9600 frames), there is no loopback to hunt with. First run from `adb shell`, as the shell user: the mic (device 13, the
bottom mic) and the output (device 3) both opened, the tick load was about 12% of a tick, one 512-frame starve at the start (the output began before the first tick; harmless), SIGINT or `--secs` drain and close.
Not yet tried with a real take (someone has to play into it) or with headphones plugged in.

## Empirical sync on the phone, 2026-11-12 (what is known, and what is not)
- **Clocks** (declared latency, 12 min): capture and playback ran at the same rate (differences of +-89 ppm are one 256-frame burst, mean zero); output ring steady; no drops, no starves. No drift between the two devices.
- **Declared latency is not to be trusted**: the guess was 9600 frames; the acoustic sync hunt (phone speaker into its bottom mic, no hardware) measured 15466 and, in a later run, 16323 frames. A 17 ms spread between two starts: the hunt has to be run on every start, as EDGE.md says.
- **Not working at the end of the session**: after those two successes (found in about 13 s each, voice-recognition input preset), five later runs (45 s to 11 min, presets voice, unprocessed, camcorder, generic) never found the round trip. The log shows the tone going to the speaker at -0.6 dB and the mic hearing it (-10 dB with the voice preset, 0 dB camcorder, -40 dB unprocessed), the sync level climbing to its loudest, and no decode. Not yet explained. Suspects: the phone's position or what was near it (noise, a hand, a surface); the preset's gain control; the level (loudest tone clips the mic; the working runs found it at about 3 bits down). Next step: a run that records the mic during the hunt to a file, to look at what the core is being given.
- `a5droid` prints per ten seconds the speaker-side and mic-side peaks and the sync level; `--hunt-every S` re-hunts every S seconds and logs each measured round trip (`HUNT n ...`).
- To do: a hunt that goes on over 30 s should say so ("cannot hear the sync tone").
