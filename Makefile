# libFLAC: FLACDIR is a source tree with include/; it is built once into build/flac.
# set FLACDIR= (empty) to use the system libflac-dev instead.
# sqlite: third_party/sqlite (run third_party/fetch.sh) if present, else the system libsqlite3-dev.
FLACDIR  ?= $(HOME)/arb/flac
FLACLIB   = build/flac/src/libFLAC/libFLAC.a
CFLAGS   ?= -O2 -Wall -Wextra
INC       = -Iinclude -Isrc

ifneq ($(FLACDIR),)
FLAC_CFLAGS = -I$(FLACDIR)/include
FLAC_LIBS   = $(FLACLIB) -lm
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

LIBOBJ = build/flacio.o build/sha256.o build/aspstore.o build/asptank.o build/aspengine.o build/gate.o build/sync.o build/settings.o build/aspcore.o
all: bin/gateprobe bin/a5pace bin/a5control bin/a5shell bin/aspflac bin/aspstore bin/asptank bin/test_store bin/test_tank bin/test_sync bin/test_core bin/test_job bin/test_mix bin/test_gate

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

build/flacio.o: src/flacio.c include/flacio.h $(FLAC_DEP)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) $(FLAC_CFLAGS) -c -o $@ $<
build/sha256.o: src/sha256.c include/aspstore.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
build/gate.o: src/gate.c include/gate.h include/asptank.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<
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
bin/gateprobe: tools/gateprobe.c $(LIBOBJ) $(SQL_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) $(SQL_CFLAGS) -o $@ $< $(LIBOBJ) $(SQL_OBJ) $(FLAC_LIBS) $(SQL_LIBS)
bin/a5control: src/a5control.c src/ctlclient.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ src/a5control.c src/ctlclient.c
bin/a5shell: src/a5shell.c src/ctlclient.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ src/a5shell.c src/ctlclient.c

bin/test_gate: tests/test_gate.c build/gate.o
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INC) -o $@ $< build/gate.o -lm

test: all bin/test_store bin/test_tank bin/test_sync bin/test_core bin/test_job
	bin/test_job
	bin/test_gate
	bin/test_mix
	tests/test_runner.sh
	bin/test_store
	bin/test_tank
	bin/test_sync
	bin/test_core
gate-diff:
	tests/gate_diff.sh $(FILES)
PREFIX ?= $(HOME)
install: bin/a5pace bin/a5control bin/a5shell
	@mkdir -p $(PREFIX)/bin
	install -m 755 bin/a5pace bin/a5control bin/a5shell $(PREFIX)/bin/
	install -m 755 tools/a5card tools/a5doctor tools/a5play $(PREFIX)/bin/
	bin/a5pace profiles install
	@echo "installed a5pace a5control a5shell a5card a5doctor a5play in $(PREFIX)/bin"
clean:
	rm -rf bin build/*.o src/schema_sql.h
.PHONY: all test clean gate-diff install
