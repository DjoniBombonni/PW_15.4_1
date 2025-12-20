# Simple Makefile for the ncurses Galaga project

TARGET ?= galaga
SRC := main.c
OBJ := $(SRC:.c=.o)

CC ?= gcc

# Use pkg-config if available (preferred), fall back to -lncurses
NCURSES_CFLAGS := $(shell pkg-config --cflags ncurses 2>/dev/null)
NCURSES_LIBS   := $(shell pkg-config --libs ncurses 2>/dev/null || echo -lncurses)

CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CFLAGS += $(NCURSES_CFLAGS)

LDFLAGS ?=
LDLIBS ?= $(NCURSES_LIBS)

.PHONY: all clean run debug

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

debug: CFLAGS += -O0 -g3
debug: clean all

clean:
	$(RM) $(OBJ) $(TARGET)
