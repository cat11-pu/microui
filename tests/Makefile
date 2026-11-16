CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -pedantic -Isrc
LDFLAGS ?=

.PHONY: all test clean

all: property_layout

property_layout: property_layout.c ../src/microui.c ../src/microui.h
	$(CC) $(CFLAGS) -o $@ property_layout.c $(LDFLAGS)

test: property_layout
	./property_layout --cases 200

clean:
	rm -f property_layout
