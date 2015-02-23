#define _XOPEN_SOURCE 700 // For strnlen().
#include <locale.h>
#include <ncurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>
#include <wchar.h>

#define max(a, b)         \
  ({ typeof(a) _a = a;    \
     typeof(b) _b = b;    \
     _a > _b ? _a : _b; })

// Checks errors for (most) ncurses functions. CHECK(fn, x, y, z) is a checked
// version of fn(x, y, z).
#define CHECK(fn, ...)                     \
  do                                       \
      if (fn(__VA_ARGS__) == ERR) {        \
          fputs(#fn"() failed\n", stderr); \
          exit(EXIT_FAILURE);              \
      }                                    \
  while (0)

static bool should_exit = false;

// Message window.
static WINDOW *msg_win;
// Separator line above the command (readline) window.
static WINDOW *sep_win;
// Command (readline) window.
static WINDOW *cmd_win;

// String displayed in the message window.
static char *msg_win_str = NULL;

// Input character for readline.
static char input;

// Used to signal "no more input" after feeding a character to readline.
static bool input_avail = false;

// Calculates the cursor position for the readline window in a way that
// supports multibyte, multi-column and combining characters. readline itself
// calculates this as part of its default redisplay function and does not
// export the cursor position.
//
// Returns the total width (in columns) of the characters in the 'n'-byte
// prefix of the null-terminated multibyte string 's'. If 'n' is larger than
// 's', returns the total width of the string. Tries to emulate how readline
// prints some special characters.
//
// 'offset' is the current horizontal offset within the line. This is used to
// get tabstops right.
//
// Makes a guess for malformed strings.
static size_t strnwidth(const char *s, size_t n, size_t offset) {
    mbstate_t shift_state;
    wchar_t wc;
    size_t wc_len;
    size_t width = 0;

    // Start in the initial shift state.
    memset(&shift_state, '\0', sizeof shift_state);

    for (size_t i = 0; i < n; i += wc_len) {
        // Extract the next multibyte character.
        wc_len = mbrtowc(&wc, s + i, MB_CUR_MAX, &shift_state);
        switch (wc_len) {
        case 0:
            // Reached the end of the string.
            goto done;

        case (size_t)-1: case (size_t)-2:
            // Failed to extract character. Guess that the remaining characters
            // are one byte/column wide each.
            width += strnlen(s, n - i);

            goto done;
        }

        if (wc == '\t')
            width = ((width + offset + 8) & ~7) - offset;
        else
            // TODO: readline also outputs ~<letter> and the like for some
            // non-printable characters.
            width += iswcntrl(wc) ? 2 : max(0, wcwidth(wc));
    }

done:
    return width;
}

// Like strnwidth, but calculates the width of the entire string.
static size_t strwidth(const char *s, size_t offset) {
    return strnwidth(s, SIZE_MAX, offset);
}

// Not bothering with 'input_avail' and just returning 0 here seems to do the
// right thing too, but this might be safer across readline versions.
static int readline_input_avail(void) {
    return input_avail;
}

static int readline_getc(FILE *dummy) {
    input_avail = false;

    return input;
}

static void forward_to_readline(char c) {
    input = c;
    input_avail = true;
    rl_callback_read_char();
}

static void msg_win_redisplay(bool for_resize) {
    CHECK(werase, msg_win);
    CHECK(mvwaddstr, msg_win, 0, 0, msg_win_str ? msg_win_str : "");

    // We batch window updates when resizing.
    if (for_resize)
        CHECK(wnoutrefresh, msg_win);
    else
        CHECK(wrefresh, msg_win);
}

static void got_command(char *line) {
    if (line == NULL)
        // Ctrl-D pressed on empty line.
        should_exit = true;
    else {
        if (*line != '\0')
            add_history(line);

        free(msg_win_str);
        msg_win_str = line;
        msg_win_redisplay(false);
    }
}

static void cmd_win_redisplay(bool for_resize) {
    size_t prompt_width = strwidth(rl_display_prompt, 0);
    size_t cursor_col = prompt_width +
                        strnwidth(rl_line_buffer, rl_point, prompt_width);

    CHECK(werase, cmd_win);
    // These can write strings wider than the terminal currently, so don't
    // check for errors.
    mvwaddstr(cmd_win, 0, 0, rl_display_prompt);
    waddstr(cmd_win, rl_line_buffer);
    if (cursor_col >= COLS)
        // Hide the cursor if it lies outside the window. Otherwise it'll
        // appear on the very right.
        CHECK(curs_set, 0);
    else {
        CHECK(wmove, cmd_win, 0, cursor_col);
        CHECK(curs_set, 1);
    }
    // We batch window updates when resizing.
    if (for_resize)
        CHECK(wnoutrefresh, cmd_win);
    else
        CHECK(wrefresh, cmd_win);
}

static void readline_redisplay(void) {
    cmd_win_redisplay(false);
}

static void resize(void) {
    if (LINES >= 3) {
        CHECK(wresize, msg_win, LINES - 2, COLS);
        CHECK(wresize, sep_win, 1, COLS);
        CHECK(wresize, cmd_win, 1, COLS);

        CHECK(mvwin, sep_win, LINES - 2, 0);
        CHECK(mvwin, cmd_win, LINES - 1, 0);

        // Batch refreshes and commit them with doupdate().
        msg_win_redisplay(true);
        CHECK(wnoutrefresh, sep_win);
        cmd_win_redisplay(true);
        CHECK(doupdate);
    }
}

static void reset_terminal(void) {
    // Avoid calling endwin() if it has already been called. Calling it a
    // second time messes with the cursor position and causes the prompt to
    // overwrite the "Shut down cleanly" message.
    if (!isendwin())
        endwin();
}

static void init_ncurses(void) {
    if (initscr() == NULL) {
        fputs("Failed to initialize ncurses\n", stderr);
        exit(EXIT_FAILURE);
    }

    // Try to clean up terminal settings on errors.
    if (atexit(reset_terminal) != 0) {
        fputs("atexit() failed\n", stderr);
        exit(EXIT_FAILURE);
    }

    CHECK(start_color);
    CHECK(use_default_colors);
    CHECK(cbreak);
    CHECK(noecho);
    nonl();
    CHECK(intrflush, NULL, FALSE);
    // Do not enable keypad() since we want to pass unadultered input to
    // readline.

    msg_win = newwin(1, 1, 0, 0);
    sep_win = newwin(1, 1, 0, 0);
    cmd_win = newwin(1, 1, 0, 0);
    if (msg_win == NULL || sep_win == NULL || cmd_win == NULL) {
        fputs("Failed to allocate windows\n", stderr);
        exit(EXIT_FAILURE);
    }

    // Allow strings longer than the message window and show only the last part
    // if the string doesn't fit.
    scrollok(msg_win, TRUE);

    // Use white-on-blue cells for the separator window.
    CHECK(init_pair, 1, COLOR_WHITE, COLOR_BLUE);
    wbkgd(sep_win, COLOR_PAIR(1));

    // Set up initial window sizes and positions.
    resize();
}

static void init_readline(void) {
    // Disable completion. TODO: Is there a more robust way to do this?
    if (rl_bind_key('\t', rl_insert) != 0) {
        fputs("Invalid key passed to rl_bind_key()\n", stderr);
        exit(EXIT_FAILURE);
    }

    // Let ncurses do all terminal and signal handling.
    rl_catch_signals = 0;
    rl_catch_sigwinch = 0;
    rl_deprep_term_function = NULL;
    rl_prep_term_function = NULL;

    // Prevent readline from setting the LINES and COLUMNS environment
    // variables, which override dynamic size adjustments in ncurses. When
    // using the alternate readline interface (as we do here), LINES and
    // COLUMNS are not updated if the terminal is resized between two calls to
    // rl_callback_read_char() (which is almost always the case).
    rl_change_environment = 0;

    // Handle input by manually feeding characters to readline.
    rl_getc_function = readline_getc;
    rl_input_available_hook = readline_input_avail;
    rl_redisplay_function = readline_redisplay;

    rl_callback_handler_install("> ", got_command);
}

static void deinit_ncurses(void) {
    delwin(msg_win);
    delwin(sep_win);
    delwin(cmd_win);
    endwin();
}

static void deinit_readline(void) {
    rl_callback_handler_remove();
}

int main(void) {
    // Set locale attributes (including encoding) from the environment.
    setlocale(LC_ALL, "");

    init_ncurses();
    init_readline();

    while (!should_exit) {
        // Using getch() here instead would refresh stdscr, overwriting the
        // initial contents of the other windows on startup.
        int c = wgetch(cmd_win);

        if (c == KEY_RESIZE || c == '\f') // '\f' == Ctrl-L.
            resize();
        else
            forward_to_readline(c);
    }

    deinit_ncurses();
    deinit_readline();

    puts("Shut down cleanly");
}
