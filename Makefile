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
MANDIR  ?= $(PREFIX)/share/man

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := bluetui

# Sources for the headless harness: everything except the UI/entry point.
LIBSRC := src/bt.c src/log.c src/strutil.c

.PHONY: all clean install uninstall debug test check

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Header dependencies
src/bt.o:      src/bt.h src/device.h src/log.h src/strutil.h
src/ui.o:      src/ui.h src/bt.h src/device.h src/log.h src/strutil.h
src/log.o:     src/log.h
src/main.o:    src/bt.h src/ui.h src/log.h
src/strutil.o: src/strutil.h src/device.h

# Debug build: AddressSanitizer + UndefinedBehaviorSanitizer, no optimisation.
# Produces ./bluetui-debug (kept separate so it never clobbers the release objs).
debug: $(SRC)
	$(CC) $(DBGFLAGS) -Isrc $(SRC) -o bluetui-debug $(DBGLIBS) -lnewt \
		-fsanitize=address,undefined

# Headless harness for debugging the BlueZ layer without the TUI.
# Built with the same sanitizers; produces ./bttest.
test: tools/bttest.c $(LIBSRC)
	$(CC) $(DBGFLAGS) -Isrc tools/bttest.c $(LIBSRC) -o bttest $(DBGLIBS) \
		-fsanitize=address,undefined

# Unit tests for the pure helpers (strutil.c): no D-Bus, no newt, no hardware.
check: tests/test_strutil.c src/strutil.c src/strutil.h src/device.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -Werror \
		tests/test_strutil.c src/strutil.c -o test_strutil
	./test_strutil

clean:
	rm -f $(OBJ) $(BIN) bluetui-debug bttest test_strutil

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 $(BIN).1 $(DESTDIR)$(MANDIR)/man1/$(BIN).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MANDIR)/man1/$(BIN).1
