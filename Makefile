MAKEFLAGS += -r
.PHONY: all clean extern
.SUFFIXES: .c .o .d
SRC := $(shell find . -path ./src -prune -o -type f -name "*.c" -print)
OBJ := $(patsubst %.c, %.o, $(SRC))
DEP := $(patsubst %.c, %.d, $(SRC))

EXT := cmacs.h include/grimoire.h lib/libcaster.a include/uv.h lib/libuv.a include/llhttp.h lib/libllhttp.a include/r3/r3.h lib/libr3.a extern/yyjson.h extern/yyjson.c
WGET := wget -qc --show-progress -t 3 --waitretry=3

CFLAGS ?= -O3 -fno-plt -pipe -flto=auto
CFLAGS += -pthread -D_REENTRANT -fwrapv -fms-extensions -Wall -Wvla -Wno-parentheses -Wno-microsoft -I$(CURDIR) -I$(CURDIR)/extern -I$(CURDIR)/include
LDFLAGS ?= -Wl,-O1
LDFLAGS += -L$(CURDIR)/lib
LDLIBS += -lm -lpthread -l:libcaster.a -l:libuv.a -l:libllhttp.a -l:libr3.a

all: main

main: $(OBJ)
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

%.o: %.c
	$(COMPILE.c) $< -o $@

clean:
	$(RM) -- $(OBJ) $(DEP) main

-include $(DEP)
%.d: %.c $(EXT)
	$(COMPILE.c) -MM $< -o $@

extern: $(EXT)
	install -d $@

cmacs.h:
	@$(WGET) -O $@ https://raw.github.com/ArcanusNEO/cmacs/master/cmacs.h || ($(RM) $@ && false)

include/grimoire.h lib/libcaster.a&:
	@$(WGET) -O libcaster.tar.gz https://api.github.com/repos/ArcanusNEO/libcaster/tarball || ($(RM) libcaster.tar.gz && false)
	install -d src/libcaster
	@tar -xf libcaster.tar.gz -C src/libcaster --strip-components=1
	@$(RM) libcaster.tar.gz
	cd src/libcaster && make -j2 && make install prefix=$(CURDIR)

include/uv.h lib/libuv.a&:
	@$(WGET) -O libuv.tar.gz https://api.github.com/repos/libuv/libuv/tarball || ($(RM) libuv.tar.gz && false)
	install -d src/libuv
	@tar -xf libuv.tar.gz -C src/libuv --strip-components=1
	@$(RM) libuv.tar.gz
	cd src/libuv && ./autogen.sh
	cd src/libuv && ./configure \
		--prefix=$(CURDIR) \
		--disable-shared \
		--enable-static \
		--disable-dependency-tracking
	cd src/libuv && make -j2 && make install

include/llhttp.h lib/libllhttp.a&:
	@$(WGET) -O llhttp.tar.gz https://api.github.com/repos/nodejs/llhttp/tarball || ($(RM) llhttp.tar.gz && false)
	install -d src/llhttp
	@tar -xf llhttp.tar.gz -C src/llhttp --strip-components=1
	@$(RM) llhttp.tar.gz
	cd src/llhttp && npm i
	cd src/llhttp && make -j2 && make install PREFIX=$(CURDIR)

include/r3/r3.h lib/libr3.a&:
	@$(WGET) -O r3.tar.gz https://api.github.com/repos/c9s/r3/tarball || ($(RM) r3.tar.gz && false)
	install -d src/r3
	@tar -xf r3.tar.gz -C src/r3 --strip-components=1
	@$(RM) r3.tar.gz
	cd src/r3 && ./autogen.sh
	cd src/r3 && ./configure \
		--prefix=$(CURDIR) \
		--disable-shared \
		--enable-static \
		--disable-dependency-tracking \
		--disable-graphviz \
		--disable-json \
		--disable-check \
		--disable-debug \
		--disable-gcov \
		--without-malloc
	cd src/r3 && make -j2 && make install

extern/yyjson.h extern/yyjson.c&:
	install -d extern
	@$(WGET) -P extern https://raw.github.com/ibireme/yyjson/master/src/yyjson.{h,c} || ($(RM) $@ && false)

.SECONDARY: $(OBJ)
