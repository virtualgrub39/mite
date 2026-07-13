SRC += mite.c

MODE ?= debug

ifeq ($(MODE),debug)
CFLAGS += -ggdb -O0
else ifeq ($(MODE),release)
CFLAGS += -O2
endif

CFLAGS += -Wall -Wextra

all: mite

mite-opcodes.h:
	./mitec header > mite-opcodes.h

mite: $(SRC) | mite-opcodes.h
	$(CC) -o $@ $(CFLAGS) $^ $(LDFLAGS)

clean:
	$(RM) mite mite-opcodes.h
