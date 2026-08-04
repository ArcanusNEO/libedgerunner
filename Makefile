MAKEFLAGS += -r
.PHONY: all clean
.SUFFIXES: .c .o .d
SRC := $(shell find . -type f -name "*.c")
OBJ := $(patsubst %.c, %.o, $(SRC))
DEP := $(patsubst %.c, %.d, $(SRC))

WGET := wget -qc --show-progress -t 3 --waitretry=3

CFLAGS ?= -O2 -fno-plt -pipe -flto=auto
CFLAGS += -pthread -D_REENTRANT -fwrapv -fms-extensions -Wall -Wvla -Wno-parentheses -Wno-microsoft -I$(CURDIR) -I$(CURDIR)/extern
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
	$(WGET) -O $@ "https://raw.github.com/ArcanusNEO/cmacs/master/cmacs.h"

extern:
	@install -d extern
# extern: extern/yyjson.h
# 	@install -d extern
# extern/yyjson.h:
# 	@install -d extern
# 	$(WGET) -O extern/yyjson.c "https://raw.github.com/ibireme/yyjson/master/src/yyjson.c"
# 	$(WGET) -O $@ "https://raw.github.com/ibireme/yyjson/master/src/yyjson.h"

.SECONDARY: $(OBJ)
