CC ?= gcc
CFLAGS ?= -g -Og
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-sign-compare \
  -Wno-unused-parameter -Wmissing-declarations -Wredundant-decls \
  -Wstrict-prototypes
override LDLIBS += -lncursesw -lreadline

rlncurses: rlncurses.c

.PHONY: clean
clean:
	$(RM) rlncurses
