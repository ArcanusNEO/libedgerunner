MAKEFLAGS += -r
.PHONY: all clean extern yyjson
.SUFFIXES: .c .o .d
SRC := $(shell find . -path src -prune -o -type f -name "*.c")
OBJ := $(patsubst %.c, %.o, $(SRC))
DEP := $(patsubst %.c, %.d, $(SRC))

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
%.d: %.c cmacs.h extern
	$(COMPILE.c) -MM $< -o $@

cmacs.h:
	$(WGET) -O $@ -- "https://raw.github.com/ArcanusNEO/cmacs/master/cmacs.h"  || (rm -f -- $@ && false)

extern: yyjson
	@install -d bin etc include lib share src extern
yyjson:
	@install -d extern
	$(WGET) -P extern -- "https://raw.github.com/ibireme/yyjson/master/src/yyjson.{c,h}" || (rm -f -- extern/yyjson.{c,h} && false)

.SECONDARY: $(OBJ)
