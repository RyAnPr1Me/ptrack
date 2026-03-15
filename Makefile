CC      ?= gcc
CFLAGS  := -Wall -Wextra -Wpedantic -std=c99 -D_POSIX_C_SOURCE=200809L \
           $(shell pkg-config --cflags libcurl 2>/dev/null || \
                   printf -- '-I/usr/include/x86_64-linux-gnu')
LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null || printf -- '-lcurl')

TARGET  := ptrack
SRCDIR  := src
SRCS    := $(SRCDIR)/ptrack.c \
           $(SRCDIR)/config.c \
           $(SRCDIR)/fetch.c  \
           $(SRCDIR)/notify.c
OBJS    := $(SRCS:.c=.o)

TESTDIR := tests
TESTS   := $(TESTDIR)/test_fetch

PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin

.PHONY: all clean install uninstall tests check

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TESTDIR)/test_fetch: $(TESTDIR)/test_fetch.c
	$(CC) $(CFLAGS) -o $@ $<

tests: $(TESTS)

check: tests
	@echo "Running tests..."
	@for t in $(TESTS); do \
	    ./$$t || exit 1; \
	done
	@echo "All tests passed."

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET) $(TESTS)

