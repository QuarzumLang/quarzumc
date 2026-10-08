LIB_PATH ?= $(CURDIR)/lib
CFLAGS  ?= -O2 -Wno-psabi

build/quarzumc: src/main.c src/tokenize.c src/string.c src/hashmap.c src/parse.c src/codegen.c src/type.c src/manifest.c
	mkdir -p build
	gcc $(CFLAGS) -DLIB_PATH=\"$(LIB_PATH)\" src/main.c src/tokenize.c src/string.c src/hashmap.c src/parse.c src/codegen.c src/type.c src/manifest.c -o build/quarzumc

# Install build: same sources, but LIB_PATH is forwarded from the install
# script (e.g. /usr/local/lib/quarzum). Kept separate so the dev binary
# (build/quarzumc, LIB_PATH=./lib) is never clobbered.
build/quarzumc-install: src/main.c src/tokenize.c src/string.c src/hashmap.c src/parse.c src/codegen.c src/type.c src/manifest.c
	mkdir -p build
	gcc $(CFLAGS) -DLIB_PATH=\"$(LIB_PATH)\" src/main.c src/tokenize.c src/string.c src/hashmap.c src/parse.c src/codegen.c src/type.c src/manifest.c -o build/quarzumc-install