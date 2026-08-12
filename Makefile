MAKEFLAGS += -r
.PHONY: all clean
.SUFFIXES: .c .o .d
SRC := $(shell find . -path ./src -prune -o -type f -name "*.c")
OBJ := $(patsubst %.c, %.o, $(SRC))
DEP := $(patsubst %.c, %.d, $(SRC))

EXT := cmacs.h extern/yyjson.h extern/yyjson.c
WGET := wget -qc --show-progress -t 3 --waitretry=3

CFLAGS ?= -O2 -fno-plt -pipe -flto=auto
CFLAGS += -pthread -D_REENTRANT -fwrapv -fms-extensions -Wall -Wvla -Wno-parentheses -Wno-microsoft -I$(CURDIR) -I$(CURDIR)/extern -I$(CURDIR)/include
LDFLAGS ?= -Wl,-O1
LDLIBS += -lm -lpthread -luv -lllhttp -lcaster

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

extern/yyjson.h extern/yyjson.c&:
	install -d extern
	@$(WGET) -P extern https://raw.github.com/ibireme/yyjson/master/src/yyjson.{h,c} || ($(RM) $@ && false)

lib/libr3.a:
	install -d src/r3 lib
	@$(WGET) -O r3.tar.gz https://api.github.com/repos/c9s/r3/tarball || ($(RM) r3.tar.gz && false)
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

.SECONDARY: $(OBJ)
