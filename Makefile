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

LIBOBJ = build/flacio.o build/sha256.o build/aspstore.o
all: bin/aspflac bin/aspstore bin/test_store

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

test: bin/test_store
	bin/test_store
clean:
	rm -rf bin build/*.o src/schema_sql.h
.PHONY: all test clean
