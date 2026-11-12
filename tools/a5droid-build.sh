#!/bin/bash
# a5droid-build.sh: cross-build the core library and examples/embed for Android arm64 with the NDK (docs/ANDROID.md).
# NDK in $A5_NDK (default ~/.local/android/android-ndk-r27c); output in build-android/ (bin/embed). API level 29 (AMidi, AAudio).
set -eu
cd "$(dirname "$0")/.."
NDK=${A5_NDK:-$HOME/.local/android/android-ndk-r27c}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TC/bin/aarch64-linux-android29-clang
FLACDIR=${FLACDIR:-$HOME/arb/flac}
O=build-android; mkdir -p $O/flac $O/obj
if [ ! -f $O/flac/src/libFLAC/libFLAC.a ]; then
  cmake -S "$FLACDIR" -B $O/flac -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DWITH_OGG=OFF -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF > /dev/null
  cmake --build $O/flac -j > /dev/null
fi
INC="-Iinclude -Isrc -I$FLACDIR/include -Ithird_party/sqlite"
[ -f src/schema_sql.h ] || make src/schema_sql.h > /dev/null
$CC -O2 -DSQLITE_THREADSAFE=1 -c -o $O/obj/sqlite3.o third_party/sqlite/sqlite3.c
objs=""
for m in codec_flac takefile keeper flacio sha256 aspstore asptank aspchip aspengine clicklib gate gatesim sync settings aspcore asppeaks; do
  $CC -O2 -Wall $INC -c -o $O/obj/$m.o src/$m.c; objs="$objs $O/obj/$m.o"
done
rm -f $O/libaspace.a; $TC/bin/llvm-ar rcs $O/libaspace.a $objs
mkdir -p $O/bin
$CC -O2 -Wall $INC -o $O/bin/embed examples/embed.c $O/libaspace.a $O/obj/sqlite3.o $O/flac/src/libFLAC/libFLAC.a -lm
file $O/bin/embed | cut -c1-120
