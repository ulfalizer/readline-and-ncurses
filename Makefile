warnings := -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter \
  -Wmissing-declarations -Wredundant-decls -Wstrict-prototypes

ncurses: ncurses.c
	gcc -std=gnu11 -g -Og $(warnings) ncurses.c -lncursesw -lreadline -o ncurses

.PHONY: clean
clean:
	rm ncurses
