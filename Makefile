CC      = gcc
CFLAGS  = -Wall -Wextra -O2

all: mkfs_builder mkfs_adder

mkfs_builder: src/mkfs_builder.c
	$(CC) $(CFLAGS) -o $@ $<

mkfs_adder: src/mkfs_adder.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	bash tests/run_tests.sh

clean:
	rm -f mkfs_builder mkfs_adder *.exe *.img

.PHONY: all test clean
