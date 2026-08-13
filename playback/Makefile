CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

# liblo via pkg-config if available, otherwise fall back to plain -llo
LO_CFLAGS := $(shell pkg-config --cflags liblo 2>/dev/null)
LO_LIBS   := $(shell pkg-config --libs liblo 2>/dev/null)
ifeq ($(strip $(LO_LIBS)),)
LO_LIBS := -llo
endif

CFLAGS += $(LO_CFLAGS)
LDLIBS += $(LO_LIBS)

.PHONY: all clean install

all: osc-jack-play

osc-jack-play: osc-jack-play.c
	$(CC) $(CFLAGS) -o $@ osc-jack-play.c $(LDLIBS)

clean:
	rm -f osc-jack-play

install: osc-jack-play
	install -m 0755 osc-jack-play $(DESTDIR)$(PREFIX)/bin/
