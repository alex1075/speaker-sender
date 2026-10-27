# speaker-send - build with `make`; needs a C11 compiler, pthreads and OpenSSL (libcrypto) headers.
#   Debian/Ubuntu: sudo apt install build-essential libssl-dev     Fedora: sudo dnf install gcc make openssl-devel
#   macOS:         brew install openssl                            (the Makefile picks up Homebrew's prefix)

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Isrc -Wall -Wextra -Wshadow -DAP_VERSION='"$(VERSION)"'
LDLIBS  += -lpthread -lm

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  BREW_SSL := $(shell brew --prefix openssl@3 2>/dev/null)
  ifneq ($(BREW_SSL),)
    CFLAGS  += -I$(BREW_SSL)/include
    LDFLAGS += -L$(BREW_SSL)/lib
  endif
endif

# STATIC=1    libcrypto linked statically, rest dynamic (glibc stays a runtime dependency)
# STATIC=full fully static executable (use with musl, e.g. Alpine: the release binaries are built that way)
ifeq ($(STATIC),full)
  LDFLAGS += -static
  LDLIBS  += -lcrypto
else ifeq ($(STATIC),1)
  ifeq ($(UNAME_S),Darwin)
    LDLIBS += $(BREW_SSL)/lib/libcrypto.a
  else
    LDLIBS += -Wl,-Bstatic -lcrypto -Wl,-Bdynamic -ldl
  endif
else
  LDLIBS += -lcrypto
endif

PREFIX ?= /usr/local
LIB_SRC := src/util.c src/bplist.c src/tlv8.c src/crypto.c src/srp.c src/alac.c src/rtsp.c src/mdns.c \
           src/airplay.c src/input.c src/stream.c
LIB_OBJ := $(LIB_SRC:.c=.o)

all: speaker-send

speaker-send: $(LIB_OBJ) src/main.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(wildcard src/*.h) $(wildcard tests/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_unit: tests/test_unit.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_mock: tests/test_mock.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests/test_ap1: tests/test_ap1.o $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

check: tests/test_unit tests/test_mock tests/test_ap1 speaker-send
	./tests/test_unit
	./tests/test_mock
	./tests/test_ap1
	./tests/test_cli.sh ./speaker-send

install: speaker-send
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 speaker-send $(DESTDIR)$(PREFIX)/bin/speaker-send

clean:
	rm -f $(LIB_OBJ) src/main.o speaker-send tests/*.o tests/test_unit tests/test_mock tests/test_ap1

.PHONY: all check install clean
