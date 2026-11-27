CC ?= clang
CFLAGS ?= -std=c23 -O2 -Wall -Wextra -Wno-unused-parameter

SRC = okc.c space-cc.c space-transform.c space_rewrite.c
HDR = space-cc.h space-transform.h space_rewrite.h

all: okc

okc: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: okc
	sh tests/run.sh

clean:
	rm -f okc

.PHONY: all test clean
