# Example demonstrating combining of readline and ncurses

## Features

Supports seamless and efficient terminal resizing and multibyte/combining/wide characters (wide characters are those that use more than one terminal column).

One of the trickier aspects is that readline does not export the cursor position (it's calculated as part of the default `rl_redisplay()` function). We have to calculate it ourselves to get special characters right.

## Limitations

* Some invalid strings and meta characters could still cause the cursor position to be off (though it's only a visual annoyance).

* Entering multibyte characters during search (e.g., **Ctrl-R**) does not work. This is due to a readline issue: http://lists.gnu.org/archive/html/bug-readline/2015-02/msg00026.html.
 
* To keep things simple, the readline (bottom) window does not scroll. It would require some care to get special characters right. ncurses pads might be handy.

## Screenshot

![screenshot](https://raw.githubusercontent.com/ulfalizer/readline-and-ncurses/screenshot/screenshot.png)

The contents of the top window is set to whatever is entered.
