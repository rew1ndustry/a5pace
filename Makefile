# libFLAC: set FLACDIR to a source/build tree with include/ and build/src/libFLAC/libFLAC.a,
# or leave empty to use the system libflac-dev (pkg-config).
FLACDIR ?= $(HOME)/arb/flac
FLACLIB ?= build/flac/src/libFLAC/libFLAC.a
ifneq ($(FLACDIR),)
FLAC_CFLAGS = -I$(FLACDIR)/include
FLAC_LIBS   = $(FLACLIB) -lm
else
FLAC_CFLAGS = $(shell pkg-config --cflags flac)
FLAC_LIBS   = $(shell pkg-config --libs flac)
endif
CFLAGS ?= -O2 -Wall -Wextra

all: bin/aspflac
bin/aspflac: src/aspflac.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(FLAC_CFLAGS) -o $@ $< $(FLAC_LIBS)
clean:
	rm -rf bin
