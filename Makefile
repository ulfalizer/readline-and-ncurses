CC := gcc
EXECUTABLE := rlncurses

warnings := -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter \
  -Wmissing-declarations -Wredundant-decls -Wstrict-prototypes

$(EXECUTABLE): rlncurses.c
	$(CC) -std=gnu11 -g $(warnings) $^ -lncursesw -lreadline -o $@

.PHONY: clean
clean:
	rm $(EXECUTABLE)
