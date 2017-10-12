// For strnlen() and wcwidth()
#define _XOPEN_SOURCE 700

#include <curses.h>
#include <locale.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define max(a, b)         \
  ({ typeof(a) _a = a;    \
     typeof(b) _b = b;    \
     _a > _b ? _a : _b; })

// Keeps track of the terminal mode so we can reset the terminal if needed on
// errors
static bool visual_mode = false;

static noreturn void fail_exit(const char *msg)
{
    // Make sure endwin() is only called in visual mode. As a note, calling it
    // twice does not seem to be supported and messed with the cursor position.
    if (visual_mode)
        endwin();
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

// Checks errors for (most) ncurses functions. CHECK(fn, x, y, z) is a checked
// version of fn(x, y, z).
#define CHECK(fn, ...)                             \
  do                                               \
      if (fn(__VA_ARGS__) == ERR)                  \
          fail_exit(#fn"("#__VA_ARGS__") failed"); \
  while (false)

static bool should_exit = false;

// Message window
static WINDOW *msg_win;
// Separator line above the command (readline) window
static WINDOW *sep_win;
// Command (readline) window
static WINDOW *cmd_win;

// String displayed in the message window
static char *msg_win_str = NULL;

// Input character for readline
static unsigned char input;

// Used to signal "no more input" after feeding a character to readline
static bool input_avail = false;

// Calculates the cursor column for the readline window in a way that supports
// multibyte, multi-column and combining characters. readline itself calculates
// this as part of its default redisplay function and does not export the
// cursor column.
//
// Returns the total width (in columns) of the characters in the 'n'-byte
// prefix of the null-terminated multibyte string 's'. If 'n' is larger than
// 's', returns the total width of the string. Tries to emulate how readline
// prints some special characters.
//
// 'offset' is the current horizontal offset within the line. This is used to
// get tab stops right.
//
// Makes a guess for malformed strings.
static size_t strnwidth(const char *s, size_t n, size_t offset)
{
    mbstate_t shift_state;
    wchar_t wc;
    size_t wc_len;
    size_t width = 0;

    // Start in the initial shift state
    memset(&shift_state, '\0', sizeof shift_state);

    for (size_t i = 0; i < n; i += wc_len) {
        // Extract the next multibyte character
        wc_len = mbrtowc(&wc, s + i, MB_CUR_MAX, &shift_state);
        switch (wc_len) {
        case 0:
            // Reached the end of the string
            goto done;

        case (size_t)-1: case (size_t)-2:
            // Failed to extract character. Guess that each character is one
            // byte/column wide each starting from the invalid character to
            // keep things simple.
            width += strnlen(s + i, n - i);
            goto done;
        }

        if (wc == '\t')
            width = ((width + offset + 8) & ~7) - offset;
        else
            // TODO: readline also outputs ~<letter> and the like for some
            // non-printable characters
            width += iswcntrl(wc) ? 2 : max(0, wcwidth(wc));
    }

done:
    return width;
}

// Like strnwidth, but calculates the width of the entire string
static size_t strwidth(const char *s, size_t offset)
{
    return strnwidth(s, SIZE_MAX, offset);
}

// Not bothering with 'input_avail' and just returning 0 here seems to do the
// right thing too, but this might be safer across readline versions
static int readline_input_avail(void)
{
    return input_avail;
}

static int readline_getc(FILE *dummy)
{
    input_avail = false;
    return input;
}

static void forward_to_readline(char c)
{
    input = c;
    input_avail = true;
    rl_callback_read_char();
}

static void msg_win_redisplay(bool for_resize)
{
    CHECK(werase, msg_win);
    CHECK(mvwaddstr, msg_win, 0, 0, msg_win_str ? msg_win_str : "");

    // We batch window updates when resizing
    if (for_resize)
        CHECK(wnoutrefresh, msg_win);
    else
        CHECK(wrefresh, msg_win);
}

static void got_command(char *line)
{
    if (!line)
        // Ctrl-D pressed on empty line
        should_exit = true;
    else {
        if (*line)
            add_history(line);

        free(msg_win_str);
        msg_win_str = line;
        msg_win_redisplay(false);
    }
}

static void cmd_win_redisplay(bool for_resize)
{
    size_t prompt_width = strwidth(rl_display_prompt, 0);
    size_t cursor_col = prompt_width +
                        strnwidth(rl_line_buffer, rl_point, prompt_width);

    CHECK(werase, cmd_win);
    // This might write a string wider than the terminal currently, so don't
    // check for errors
    mvwprintw(cmd_win, 0, 0, "%s%s", rl_display_prompt, rl_line_buffer);
    if (cursor_col >= COLS)
        // Hide the cursor if it lies outside the window. Otherwise it'll
        // appear on the very right.
        curs_set(0);
    else {
        CHECK(wmove, cmd_win, 0, cursor_col);
        curs_set(2);
    }
    // We batch window updates when resizing
    if (for_resize)
        CHECK(wnoutrefresh, cmd_win);
    else
        CHECK(wrefresh, cmd_win);
}

static void readline_redisplay(void)
{
    cmd_win_redisplay(false);
}

static void resize(void)
{
    if (LINES >= 3) {
        CHECK(wresize, msg_win, LINES - 2, COLS);
        CHECK(wresize, sep_win, 1, COLS);
        CHECK(wresize, cmd_win, 1, COLS);

        CHECK(mvwin, sep_win, LINES - 2, 0);
        CHECK(mvwin, cmd_win, LINES - 1, 0);
    }

    // Batch refreshes and commit them with doupdate()
    msg_win_redisplay(true);
    CHECK(wnoutrefresh, sep_win);
    cmd_win_redisplay(true);
    CHECK(doupdate);
}

static void init_ncurses(void)
{
    if (!initscr())
        fail_exit("Failed to initialize ncurses");
    visual_mode = true;

    if (has_colors()) {
        CHECK(start_color);
        CHECK(use_default_colors);
    }
    CHECK(cbreak);
    CHECK(noecho);
    CHECK(nonl);
    CHECK(intrflush, NULL, FALSE);
    // Do not enable keypad() since we want to pass unadulterated input to
    // readline

    // Explicitly specify a "very visible" cursor to make sure it's at least
    // consistent when we turn the cursor on and off (maybe it would make sense
    // to query it and use the value we get back too). "normal" vs. "very
    // visible" makes no difference in gnome-terminal or xterm. Let this fail
    // for terminals that do not support cursor visibility adjustments.
    curs_set(2);

    if (LINES >= 3) {
        msg_win = newwin(LINES - 2, COLS, 0, 0);
        sep_win = newwin(1, COLS, LINES - 2, 0);
        cmd_win = newwin(1, COLS, LINES - 1, 0);
    }
    else {
        // Degenerate case. Give the windows the minimum workable size to
        // prevent errors from e.g. wmove().
        msg_win = newwin(1, COLS, 0, 0);
        sep_win = newwin(1, COLS, 0, 0);
        cmd_win = newwin(1, COLS, 0, 0);
    }
    if (!msg_win || !sep_win || !cmd_win)
        fail_exit("Failed to allocate windows");

    // Allow strings longer than the message window and show only the last part
    // if the string doesn't fit
    CHECK(scrollok, msg_win, TRUE);

    if (has_colors()) {
        // Use white-on-blue cells for the separator window...
        CHECK(init_pair, 1, COLOR_WHITE, COLOR_BLUE);
        CHECK(wbkgd, sep_win, COLOR_PAIR(1));
    }
    else
        // ...or the "best highlighting mode of the terminal" if it doesn't
        // support colors
        CHECK(wbkgd, sep_win, A_STANDOUT);
    CHECK(wrefresh, sep_win);
}

static void deinit_ncurses(void)
{
    CHECK(delwin, msg_win);
    CHECK(delwin, sep_win);
    CHECK(delwin, cmd_win);
    CHECK(endwin);
    visual_mode = false;
}

static void init_readline(void)
{
    // Disable completion. TODO: Is there a more robust way to do this?
    if (rl_bind_key('\t', rl_insert))
        fail_exit("Invalid key passed to rl_bind_key()");

    // Let ncurses do all terminal and signal handling
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

    // Handle input by manually feeding characters to readline
    rl_getc_function = readline_getc;
    rl_input_available_hook = readline_input_avail;
    rl_redisplay_function = readline_redisplay;

    rl_callback_handler_install("> ", got_command);
}

static void deinit_readline(void)
{
    rl_callback_handler_remove();
}

int main(void)
{
    // Set locale attributes (including encoding) from the environment
    if (!setlocale(LC_ALL, ""))
        fail_exit("Failed to set locale attributes from environment");

    init_ncurses();
    init_readline();

    do {
        // Using getch() here instead would refresh stdscr, overwriting the
        // initial contents of the other windows on startup
        int c = wgetch(cmd_win);

        switch (c) {
        case KEY_RESIZE:
            resize();
            break;

        // Ctrl-L -- redraw screen
        case '\f':
            // Makes the next refresh repaint the screen from scratch
            CHECK(clearok, curscr, TRUE);
            // Resize and reposition windows in case that got messed up somehow
            resize();
            break;

        default:
            forward_to_readline(c);
        }
    } while (!should_exit);

    deinit_ncurses();
    deinit_readline();

    puts("Shut down cleanly");
}
