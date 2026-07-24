CC      ?= cc
# Append (not override) so distro/debhelper-supplied CFLAGS/CPPFLAGS/LDFLAGS
# survive — needed for a policy-compliant Debian build with hardening flags.
CFLAGS   += -std=c11 -O2 -Wall -Wextra
CPPFLAGS += $(shell pkg-config --cflags libsystemd)
LDLIBS   += $(shell pkg-config --libs libsystemd) -lnewt

# Sanitizer flags shared by the `debug` and `test` targets.
DBGFLAGS := -std=c11 -g -O0 -Wall -Wextra -fsanitize=address,undefined \
            $(shell pkg-config --cflags libsystemd)
DBGLIBS  := $(shell pkg-config --libs libsystemd)

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := blttui

# Sources for the headless harness: everything except the UI/entry point.
LIBSRC := src/bt.c src/log.c

.PHONY: all clean install uninstall debug test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Header dependencies
src/bt.o:   src/bt.h src/device.h src/log.h
src/ui.o:   src/ui.h src/bt.h src/device.h src/log.h
src/log.o:  src/log.h
src/main.o: src/bt.h src/ui.h src/log.h

# Debug build: AddressSanitizer + UndefinedBehaviorSanitizer, no optimisation.
# Produces ./blttui-debug (kept separate so it never clobbers the release objs).
debug: $(SRC)
	$(CC) $(DBGFLAGS) -Isrc $(SRC) -o blttui-debug $(DBGLIBS) -lnewt \
		-fsanitize=address,undefined

# Headless harness for debugging the BlueZ layer without the TUI.
# Built with the same sanitizers; produces ./bttest.
test: tools/bttest.c $(LIBSRC)
	$(CC) $(DBGFLAGS) -Isrc tools/bttest.c $(LIBSRC) -o bttest $(DBGLIBS) \
		-fsanitize=address,undefined

clean:
	rm -f $(OBJ) $(BIN) blttui-debug bttest

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
