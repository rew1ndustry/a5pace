# libFLAC: FLACDIR is a source tree with include/; it is built once into build/flac.
# set FLACDIR= (empty) to use the system libflac-dev instead.
# sqlite: third_party/sqlite (run third_party/fetch.sh) if present, else the system libsqlite3-dev.
FLACDIR  ?= $(HOME)/arb/flac
FLACLIB   = build/flac/src/libFLAC/libFLAC.a
CFLAGS   ?= -O2 -Wall -Wextra
INC       = -Iinclude -Isrc

ifneq ($(FLACDIR),)
FLAC_CFLAGS = -I$(FLACDIR)/include
FLAC_LIBS   = $(FLACLIB) -lm -lpthread
FLAC_DEP    = $(FLACLIB)
else
FLAC_CFLAGS = $(shell pkg-config --cflags flac)
FLAC_LIBS   = $(shell pkg-config --libs flac)
FLAC_DEP    =
endif

ifneq ($(wildcard third_party/sqlite/sqlite3.c),)
SQL_CFLAGS = -Ithird_party/sqlite
SQL_OBJ    = build/sqlite3.o
SQL_LIBS   = -lpthread -ldl -lm
else
SQL_CFLAGS =
SQL_OBJ    =
SQL_LIBS   = -lsqlite3
endif

# the codec, chosen at build time: flac (default) or raw. see include/codec.h
CODEC   ?= flac
ifeq ($(CODEC),flac)
CODEC_CFLAGS = $(FLAC_CFLAGS)
CODEC_DEP    = $(FLAC_DEP)
CODEC_LIBS   = $(FLAC_LIBS)
else
CODEC_CFLAGS =
CODEC_DEP    =
CODEC_LIBS   = -lm
endif

LIBOBJ = build/codec_$(CODEC).o build/takefile.o build/keeper.o build/flacio.o build/sha256.o build/aspstore.o build/asptank.o build/aspengine.o build/clicklib.o build/gate.o build/gatesim.o build/sync.o build/settings.o build/aspcore.o
all: bin/a5create bin/a5click bin/a5export bin/a5import bin/a5recover bin/a5tune bin/gateprobe bin/a5pace bin/a5control bin/a5hell bin/aspflac bin/aspstore bin/asptank bin/test_store bin/test_tank bin/test_sync bin/test_core bin/test_job bin/test_mix bin/test_gate bin/test_codec bin/test_takefile bin/test_keeper

$(FLACLIB):
	cmake -S $(FLACDIR) -B build/flac -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DWITH_OGG=OFF \
	  -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
	  -DINSTALL_MANPAGES=OFF > /dev/null
	cmake --build build/flac -j > /dev/null

src/schema_sql.h: tools/schema.sql
	awk 'BEGIN{print "static const char *SCHEMA_SQL ="} {gsub(/\\/,"\\\\");gsub(/"/,"\\\"");print "\"" $$0 "\\n\""} END{print ";"}' $< > $@

build/sqlite3.o: third_party/sqlite/sqlite3.c
	@mkdir -p build
	$(CC) -O2 -DSQLITE_THREADSAFE=1 -c -o $@ $<

# changing CODEC makes a new stamp, which rebuilds the codec, which relinks everything that uses it
CODEC_STAMP = build/codec.$(CODEC).stamp
$(CODEC_STAMP):
	@mkdir -p build
	@rm -f build/codec.*.stamp
	@touch $@
build/codec_$(CODEC).o: src/codec_$(CODEC).c include/codec.h include/plat.h $(CODEC_DEP) $(CODEC_STAMP)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) $(CODEC_CFLAGS) -c -o $@ $<
build/keeper.o: src/keeper.c include/keeper.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -c -o $@ $<
build/takefile.o: src/takefile.c include/takefile.h include/codec.h include/asptank.h include/plat.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
bin/test_takefile: tests/test_takefile.c build/takefile.o build/codec_$(CODEC).o include/takefile.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< build/takefile.o build/codec_$(CODEC).o $(CODEC_LIBS)
bin/test_keeper: tests/test_keeper.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/test_codec: tests/test_codec.c build/codec_$(CODEC).o include/codec.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< build/codec_$(CODEC).o $(CODEC_LIBS)

build/flacio.o: src/flacio.c include/flacio.h $(FLAC_DEP)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) $(FLAC_CFLAGS) -c -o $@ $<
build/sha256.o: src/sha256.c include/aspstore.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
# the gate is mostly single precision fft: on a cortex-a8 (the cubieboard) the vfp is slow and neon is not used for floats
# unless told it may reassociate; that is 2.5 times faster there. other machines are left alone.
ifneq ($(filter armv7%,$(shell uname -m)),)
GATE_CFLAGS = -O3 -mfpu=neon -funsafe-math-optimizations -fno-math-errno
endif
build/gate.o: src/gate.c include/gate.h include/asptank.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(GATE_CFLAGS) $(INC) -c -o $@ $<
build/settings.o: src/settings.c include/settings.h include/aspcore.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/aspcore.o: src/aspcore.c include/settings.h include/aspcore.h include/aspstore.h include/asptank.h include/sync.h include/gate.h include/flacio.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/sync.o: src/sync.c include/sync.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-misleading-indentation -Wno-sign-compare $(INC) -c -o $@ $<
build/asptank.o: src/asptank.c include/asptank.h include/plat.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/aspengine.o: src/aspengine.c include/aspengine.h include/aspstore.h include/asptank.h include/flacio.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/aspstore.o: src/aspstore.c include/aspstore.h include/flacio.h src/schema_sql.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -c -o $@ $<

bin/aspflac: src/aspflac.c $(LIBOBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(FLAC_CFLAGS) -o $@ $< build/flacio.o $(FLAC_LIBS)
bin/aspstore: src/aspstore_cli.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/test_store: tests/test_store.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)

bin/asptank: src/asptank_cli.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/test_tank: tests/test_tank.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)

bin/test_sync: tests/test_sync.c build/sync.o
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< build/sync.o -lm

bin/test_core: tests/test_core.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)

bin/test_job: tests/test_job.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)

bin/test_mix: tests/test_mix.c src/simedge.h $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -Wno-parentheses $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)

bin/a5pace: src/aspace_main.c src/ctlsrv.c src/simedge.h $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -Wno-unused-function $(INC) $(SQL_CFLAGS) -o $@ src/aspace_main.c src/ctlsrv.c $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/tickbench: tools/tickbench.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/gateprobe: tools/gateprobe.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
build/gatesim.o: src/gatesim.c include/gatesim.h include/gate.h include/asptank.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/clicklib.o: src/clicklib.c include/clicklib.h include/aspstore.h include/asptank.h include/aspengine.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
bin/a5create: src/a5create.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/a5click: src/a5click.c src/ctlclient.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ src/a5click.c src/ctlclient.c $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/a5import: src/a5import.c src/ctlclient.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ src/a5import.c src/ctlclient.c $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/a5recover: src/a5recover.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ src/a5recover.c $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/a5export: src/a5export.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ src/a5export.c $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/a5tune: src/a5tune.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -Wno-unused-function $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/a5control: src/a5control.c src/ctlclient.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ src/a5control.c src/ctlclient.c
bin/a5hell: src/a5hell.c src/a5mixer.c src/a5mixer.h src/ctlclient.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ src/a5hell.c src/a5mixer.c src/ctlclient.c

bin/test_mixparse: tests/test_mixparse.c src/a5mixer.c src/a5mixer.h src/ctlclient.c
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_mixparse.c src/a5mixer.c src/ctlclient.c

bin/test_gate: tests/test_gate.c build/gate.o
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< build/gate.o -lm

# the tests make their own artist, not yours
test: export XDG_CONFIG_HOME = $(CURDIR)/build/testcfg
test: all bin/test_keeper bin/test_codec bin/test_takefile bin/test_store bin/test_tank bin/test_sync bin/test_core bin/test_job bin/test_mixparse
	bin/test_codec
	bin/test_takefile
	bin/test_keeper
	bin/test_job
	bin/test_mixparse
	bin/test_gate
	bin/test_mix
	tests/test_runner.sh
	tests/test_cal.sh
	tests/test_click.sh
	tests/test_export.sh
	tests/test_import.sh
	tests/test_start.sh
	tests/test_recover.sh
	bin/test_store
	bin/test_tank
	bin/test_sync
	bin/test_core
gate-diff:
	tests/gate_diff.sh $(FILES)
PREFIX ?= $(HOME)
install: bin/a5pace bin/a5control bin/a5hell bin/a5tune bin/a5create bin/a5click bin/a5export bin/a5import bin/a5recover bin/aspflac
	@mkdir -p $(PREFIX)/bin
	install -m 755 bin/a5pace bin/a5control bin/a5hell $(PREFIX)/bin/
	install -m 755 tools/a5card tools/a5doctor tools/a5play tools/a5cal tools/a5start $(PREFIX)/bin/
	install -m 755 bin/a5tune bin/a5create bin/a5click bin/a5export bin/a5import bin/a5recover $(PREFIX)/bin/
	install -m 755 bin/aspflac $(PREFIX)/bin/a5flac
	bin/a5pace profiles install
	@echo "installed a5pace a5control a5hell a5card a5doctor a5play a5cal a5start a5tune a5create a5click a5export a5import a5recover a5flac in $(PREFIX)/bin"
clean:
	rm -rf bin build/*.o src/schema_sql.h
.PHONY: all test clean gate-diff install
