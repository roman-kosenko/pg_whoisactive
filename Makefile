CC = gcc

CFLAGS = -std=c17 -O2 -Wall -Wextra -pedantic

PKG_CFLAGS := $(shell pkg-config --cflags libpq ncurses)
PKG_LIBS   := $(shell pkg-config --libs libpq ncurses)

INCLUDES = -Iinclude $(PKG_CFLAGS)

SRC = \
	src/main.c \
	src/plain.c \
	src/ui.c \
	src/procfs.c \
	src/postgres.c \
    src/metrics.c

OBJ = $(SRC:.c=.o)

TARGET = pg_whoisactive

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(PKG_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean