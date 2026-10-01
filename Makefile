EXE=asyncio
CC=gcc
CFLAGS=
SOURCE=asyncio.c

ver=debug

ifeq ($(ver), debug)
CFLAGS+=-g -fsanitize=address -fno-omit-frame-pointer -O0
else
endif

all: $(EXE)

$(EXE): $(SOURCE)
	$(CC) $(CFLAGS) $< -o $@ -luring

debug:

clean:
	rm -rf $(EXE)

.PHONY: all clean
