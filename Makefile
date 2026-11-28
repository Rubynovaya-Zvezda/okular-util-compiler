CC ?= clang
CFLAGS ?= -std=c23 -O2 -Wall -Wextra -Wno-unused-parameter

SRC = okc.c space-cc.c space-lsp.c space-transform.c space_rewrite.c
HDR = space-cc.h space-lsp.h space-transform.h space_rewrite.h

all: okc

okc: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: okc
	sh tests/run.sh

test-js:
	cd coc-okc && node test/basic.js

publish-coc:
	cd coc-okc && npm publish

PREFIX ?= /usr/local
install:
	bazel run //:install_okc -- --prefix=$(PREFIX)

clean:
	rm -f okc

.PHONY: all test test-js publish-coc install clean
