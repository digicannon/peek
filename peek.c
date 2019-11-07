/* Copyright (C) 2019  Noah Greenberg

   This file is part of Peek.

   Peek is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Peek is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "wcwidth.h"

#ifndef DEBUG
#ifndef RELEASE
#define DEBUG 1
#else
#define DEBUG 0
#endif
#endif

#if (DEBUG == 0)
#warning Building release build!
#endif

#define ANSI_CURSOR_UP    "\e[%dA"
#define ANSI_CURSOR_DOWN  "\e[%dB"
#define ANSI_CURSOR_RIGHT "\e[%dC"
#define ANSI_CURSOR_LEFT  "\e[%dD"

#define ANSI_ERASE_TO_DISP_END   "\e[0J"
#define ANSI_ERASE_TO_DISP_START "\e[1J"
#define ANSI_ERASE_DISP          "\e[2J"

#define ANSI_ERASE_TO_LINE_END   "\e[0K"
#define ANSI_ERASE_TO_LINE_START "\e[1K"
#define ANSI_ERASE_LINE          "\e[2K"

#define ANSI_ERASE_ALL_AHEAD     "\e[0J\e[2K"

#define ANSI_RESET  "\e[m"
#define ANSI_BOLD   "\e[1m"
#define ANSI_INVERT "\e[7m"

#define ANSI_CURSOR_SHOW "\e[?25h"
#define ANSI_CURSOR_HIDE "\e[?25l"

#define VERSION "0.2.1"
#if (DEBUG == 1)
#define MSG_VERSION "Peek " VERSION "-debug\n"
#else
#define MSG_VERSION "Peek " VERSION "\n"
#endif

#define SHORT_FLAGS "AaBcFohv"
#define MSG_USAGE   "Usage: %s [-" SHORT_FLAGS "] [<directory>]"
#define MSG_INVALID MSG_USAGE "\nTry '%s -h' for more information.\n"
#define MSG_HELP MSG_USAGE "\nInteractive exploration of directories on the command line.\n"              \
                           "\nFlags:\n"                                                                   \
                           "  -A\tShow files starting with . (hidden by default).\n"                      \
                           "  -a\tDuplicate of -a.\n"                                                     \
                           "  -B\tDon't output color.\n"                                                  \
                           "  -c\tClear listing on exit.  Ignored with -o.\n"                             \
                           "  -F\tAppend ls style indicators to the end of entries.\n"                    \
                           "  -o\tPrint listing and exit.  AKA LS mode.\n"                                \
                           "  -h\tPrint this message and exit.\n"                                         \
                           "  -v\tPrint version and exit.\n"                                              \
                           "\nNormal Mode:\n"                                                             \
                           "   F10|Q \tQuit.\n"                                                           \
                           "   BS|DEL\tOpen parent directory.\n"                                          \
                           "   Enter \tOpen selected directory.\n"                                        \
                           "   Up|K   \tMove cursor up.\n"                                                \
                           "   Down|J \tMove cursor down.\n"                                              \
                           "   Left|H \tMove cursor left.\n"                                              \
                           "   Right|L\tMove cursor right.\n"                                             \
                           "   E\tEdit selected entry.\n"                                                 \
                           "   O\tOpen selected entry.\n"                                                 \
                           "   R\tRefresh directory listing.\n"                                           \
                           "   S\tOpen shell.\n"                                                          \
                           "   X\tExecute selected entry.\n"                                              \
                           "   /\tSearch mode.\n"                                                         \
                           "\nSearch Mode:\n"                                                             \
                           "   Escape\tEnd search.\n"                                                     \
                           "   Enter \tEnd search and open matched directory.\n"
#define MSG_CANT_SCAN "could not scan"
#define MSG_EMPTY     "empty"

// 8.3 was FAT's max filename.  That sounds like a good minimum.
#define MIN_ENTRY_LEN   11
#define ENTRY_DELIM     "  "
#define ENTRY_DELIM_LEN 2

// Enivronment variables to set when executing a process.
#define ENV_NAME_CHILD_ID "PK_CHILD"
#define ENV_NAME_SELECTED "PK_FILE"

// The program to open files.  OS dependant.
#ifndef EXEC_NAME_OPENER
    #if defined(__CYGWIN__)
        #define EXEC_NAME_OPENER "cygstart"
    #elif defined(__APPLE__) && defined(__MACH__)
        #define EXEC_NAME_OPENER "open"
    #elif defined(__unix__)
        #define EXEC_NAME_OPENER "xdg-open"
    #else
        #define EXEC_NAME_OPENER NULL
    #endif
#endif

// The program to edit files in the terminal.
#ifndef EXEC_NAME_EDITOR
    #define EXEC_NAME_EDITOR "vim"
#endif

typedef enum user_action {
    USER_ACT_MV_UP,
    USER_ACT_MV_DOWN,
    USER_ACT_MV_LEFT,
    USER_ACT_MV_RIGHT,
    USER_ACT_CD_PARENT,
    USER_ACT_CD_SELECT,
    USER_ACT_CD_RELOAD,
    USER_ACT_ON_EDIT,
    USER_ACT_ON_EXEC,
    USER_ACT_ON_OPEN,
    USER_ACT_SEARCH,
    USER_ACT_SHELL,
} user_action;

typedef struct peek_entry {
    int len; // Printed UTF8 length, not number of bytes.
    const char * color;
    char indicator;
    int cells_down;
    int cells_over;
} peek_entry;

enum prompt_t {
    PROMPT_NONE,
    PROMPT_ERR,
    PROMPT_MSG,
    PROMPT_CMD,
    PROMPT_SEARCH,
} prompt = PROMPT_NONE;

static char * current_dir     = NULL;
static size_t current_dir_len = 0;

static struct dirent ** posix_entries = NULL;
static peek_entry *     entry_data    = NULL;
static int              entry_count   = 0; // Number of entries in current dir.

static bool display_is_dirty = true; // Force display redraw when true.
static int  entry_row_offset = 0;

static bool formatted;     // If true, output will do column formatting.
static int  total_length;  // Length of output without newlines.
static int  entry_columns; // Number of entries per line, if formatted.
static int  entry_lines;   // Number of lines taken by entries, printed or not.
static int  newline_count; // Number of lines printed by last display.

static int * entry_column_widths; // The longest entry in each column.

// Used for limiting display to a portion of the listing.
static int i_offset;
static int i_limit;

#define SELECTED_NOT -1
#define SELECTED_MIN 0
#define SELECTED_MAX (entry_count - 1)
static int selected            = SELECTED_MIN;
static int selected_previously = SELECTED_NOT;
// TODO: This size should not be assumed anymore.
#define SELECTED_MAXLEN 256
static char selected_name[SELECTED_MAXLEN];

#define PROMPT_MAXLEN 80
static char prompt_buffer[PROMPT_MAXLEN];
static size_t prompt_buffer_i = 0;

static struct termios tcattr_old;
static struct termios tcattr_raw;
static struct winsize termsize;

static bool cfg_show_dotfiles = 0; //  (-a) If set, files starting with . will be shown.
static bool cfg_color         = 1; // !(-B) If set, color output.
static bool cfg_clear_trace   = 0; //  (-c) If set, clear displayed text on exit.
static bool cfg_indicate      = 0; //  (-F) If set, append indicators to entries.
static bool cfg_oneshot       = 0; //  (-o) If set, print listing and exit.  (AKA LS mode.)

// The value here is the value if getenv("SHELL") returns NULL.
// /bin/sh is guaranteed by POSIX to exist.
static char * cfg_shell_path = "/bin/sh";

static void restore_tcattr() {
    printf(ANSI_CURSOR_SHOW);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_old);
}

static void restore_tcattr_and_clean() {
    if (cfg_clear_trace) {
        printf(ANSI_ERASE_ALL_AHEAD);
    } else {
        // Move down a line for every line printed.
        printf(ANSI_CURSOR_DOWN "\n", newline_count);
    }

    restore_tcattr();
}

// Create raw terminal mode to stop stdin buffer from breaking key press detection.
static void replace_tcattr() {
    static bool first_time = true;

    if (first_time) {
        first_time = false;

        atexit(restore_tcattr_and_clean); // Restore old mode when we're done.

        tcgetattr(STDIN_FILENO, &tcattr_old);
        memcpy(&tcattr_raw, &tcattr_old, sizeof(struct termios));
        tcattr_raw.c_cc[VMIN]  = 1;
        tcattr_raw.c_cc[VTIME] = 0;
        tcattr_raw.c_lflag &= ~(ECHO | ICANON);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_raw);
    printf(ANSI_CURSOR_HIDE);
}

static int display_filter(const struct dirent * ent) {
    if (ent->d_name[0] == '.') {
        if (!cfg_show_dotfiles) return 0;
        else if (ent->d_name[1] == 0) return 0; // Don't show "."
        else if (ent->d_name[1] == '.' && ent->d_name[2] == 0) return 0; // Don't show ".."
    }
    return 1;
}

static int utf8_len(unsigned char * str) {
    int len = 0;

    uint32_t unicode = 0; // Complete unicode code point.
    int uni_bytes = 0;    // How many bytes of the code point are left to read.

    for (unsigned char * codepoint = str; *codepoint; ++codepoint) {
        char c = *codepoint;

        if (!(c & 0x80)) {
            // The MSB is 0, so this is an ASCII character.
            len += mk_wcwidth(c);
        } else if ((c & 0xC0) == 0x80) {
            // This byte is part of a multibyte codepoint.
            unicode = unicode << 6; // Bytes 2-4 are 6 bits.
            unicode |= c & 0x3F;    // OR in those 6 bits.
            if (--uni_bytes == 0) len += mk_wcwidth(unicode);
        } else {
            // This is the start of a multibyte codepoint.

            if ((c & 0xE0) == 0xC0) {
                // MSBs are 110, starting a 2 byte codepoint.
                unicode = c & 0x1F; // 5 LSBs.
                uni_bytes = 1;
            } else if ((c & 0xF0) == 0xE0) {
                // MSBs are 1110, starting a 3 byte codepoint.
                unicode = c & 0xF; // 4 LSBs.
                uni_bytes = 2;
            } else if ((c & 0xF8) == 0xF0) {
                // MSBs are 11110, starting a 4 byte codepoint.
                unicode = c & 7; // 3 LSBs.
                uni_bytes = 3;
            }
        }
    }

    return len;
}

// getcwd, but without an existing buffer.
// The buffer will resize until it can fit
// the current working directory, even if
// it is larger than PATH_MAX.
static char * sturdy_getcwd() {
    size_t size  = PATH_MAX;
    char * path  = NULL;
    char * valid = NULL;

    for (; !valid; size += PATH_MAX) {
        path  = realloc(path, size);
        valid = getcwd(path, size);

        if (!valid && errno != ERANGE) {
            return NULL;
        }
    }

    return path;
}

static bool sturdy_chdir(char * path) {
    // TODO: If path is longer than PATH_MAX, this will fail.
    // If it does, break it up until it works.

    return chdir(path) == 0;
}

static void get_entry_type(struct dirent * ent, const char ** color, char * indicator) {
    static const char * colors[] = {
        0,          // DT_UNKNOWN
        "\e[33m",   // DT_FIFO
        "\e[33;1m", // DT_CHR
        0,
        "\e[34;1m", // DT_DIR
        0,
        "\e[33;1m", // DT_BLK
        0,
        0,          // DT_REG
        0,
        "\e[36;1m", // DT_LNK
        0,
        "\e[35;1m", // DT_SOCK
    };
    static const char indicators[] = "\0|\0\0/\0\0\0\0\0@\0=";

    if (ent->d_type > 0 && ent->d_type <= DT_SOCK
        && (colors[ent->d_type] || indicators[ent->d_type])) {
        *color     = colors[ent->d_type];
        *indicator = indicators[ent->d_type];
    } else {
        // d_type couldn't tell us anything, so check if executable.
        if (access(ent->d_name, X_OK) == 0) {
            *color     = "\e[32;1m";
            *indicator = '*';
        } else {
            *color     = 0;
            *indicator = 0;
        }
    }
}

static void run_scan() {
    int old_entry_count = entry_count;
    int len = 0;

    // The next refresh needs to know that the data on screen is no longer valid.
    display_is_dirty = true;

    entry_count = scandir(current_dir, &posix_entries, display_filter, alphasort);
    if (entry_count <= 0) {
        selected_name[0] = 0;
        return;
    }

    if (entry_data == NULL || entry_count > old_entry_count) {
        entry_data = realloc(entry_data, sizeof(*entry_data) * entry_count);
    }

    formatted    = 1;
    total_length = 0;

    for (int i = 0; i < entry_count; ++i) {
        entry_data[i].len = utf8_len((unsigned char *)posix_entries[i]->d_name);
        len = entry_data[i].len;

        get_entry_type(posix_entries[i], &entry_data[i].color, &entry_data[i].indicator);
        if (!cfg_color)    entry_data[i].color     = 0;
        if (!cfg_indicate) entry_data[i].indicator = 0;

        total_length += len;
        if (entry_data[i].indicator) ++total_length;
        total_length += ENTRY_DELIM_LEN;
    }
}

static void perform_search() {
    const char * search_ptr;
    const char * entry_ptr;

    for (int i = 0; i < entry_count; ++i) {
        search_ptr = prompt_buffer;
        entry_ptr  = posix_entries[i]->d_name;

        for (; *search_ptr && *entry_ptr; ++search_ptr, ++entry_ptr) {
            if (*search_ptr != *entry_ptr) goto next;
        }

        selected_previously = selected;
        selected = i;
        return;

next:
        continue;
    }
}

static void free_posix_entries() {
    if (posix_entries) {
        for (int i = 0; i < entry_count; ++i) free(posix_entries[i]);
        free(posix_entries);
        posix_entries = NULL;
        display_is_dirty = true;
    }
}

static void cd(char * to) {
    if (!sturdy_chdir(to)) {
        sprintf(prompt_buffer, "%s", strerror(errno));
        prompt = PROMPT_ERR;
        return;
    }

    if (current_dir) free(current_dir);

    if ((current_dir = sturdy_getcwd()) == NULL) {
        // TODO: This is fatal.  Do something to communicate.
        exit(1);
    }

    current_dir_len = strlen(current_dir);

    free_posix_entries();

    selected            = SELECTED_MIN;
    selected_previously = SELECTED_NOT;
}

// NOTE: Allocated with malloc & should be freed.
static char * get_selected_fullpath() {
    char * p = malloc(sizeof(*current_dir)
                      * (current_dir_len + 1 + strlen(selected_name) + 1));
    sprintf(p, "%s/%s", current_dir, selected_name);
    return p;
}

// Make sure the selection isn't out of bounds.
static void validate_selection_index() {
    if (entry_count < 1) selected = 0;
    else if (selected < SELECTED_MIN) selected = SELECTED_MIN;
    else if (selected > SELECTED_MAX) selected = SELECTED_MAX;

    if (selected_previously > SELECTED_MAX) selected_previously = SELECTED_NOT;

    // For partial displays, a renew must occur when the cursor
    // passes the portion we are already displaying.
    if (selected < i_offset || selected > i_limit) {
        display_is_dirty = true;
    }
}

static int write_entry(int index, int width) {
    struct dirent * d_child           = posix_entries[index];
    const char *    d_child_color     = entry_data[index].color;
    char            d_child_indicator = entry_data[index].indicator;

    int used_chars = 0;

    // If enabled, print the corresponding color for the type.
    if (d_child_color) printf("%s", d_child_color);
    
    // Print the name of the entry.
    for (unsigned char * c = (unsigned char *)d_child->d_name; *c; ++c) {
        // Don't print ACII control characters.
        if (*c >= 32 && *c != 0x7F) putchar(*c);
    }
    printf(ANSI_RESET);
    used_chars += entry_data[index].len;

    // If enabled, print the corresponding indicator for the type.
    if (d_child_indicator) {
        putchar(d_child_indicator);
        ++used_chars;
    }

    if (formatted) {
        for (; used_chars < width; ++used_chars) putchar(' ');
    } else {
        used_chars += printf(ENTRY_DELIM);
    }

    return used_chars;
}

// If write_widths is set, each column width
// will be written to entry_column_widths[].
// It is expected that entry_column_widths
// has been allocated the appropriate size.
// In addition, the write will be cut short
// if cols if found to be an invalid amount.
static bool valid_column_count(int cols, bool write_widths) {
    int lines = (entry_count - 1) / cols + 1;
    int width = 0;

    // Find the longest entry in each line.
    // Keep adding the longest entry to the width
    // and we'll have how wide our output would be.

    for (int col = 0; col < cols; ++col) {
        int longest = 0;

        for (int line = 0; line < lines; ++line) {
            int i = line * cols + col;
            int len;

            if (i >= entry_count) break;
            len = entry_data[i].len;
            if (entry_data[i].indicator) ++len;
            if (len > longest) longest = len;
        }

        if (col < cols - 1) longest += ENTRY_DELIM_LEN;
        if (write_widths) entry_column_widths[col] = longest;
        width += longest;
        if (width >= termsize.ws_col) return false;
    }

    return true;
}

static void renew_display() {
    // If formatting, this will be the next format column to use.
    int next_column = 0;
    // The amount of characters printed in the current line.
    int used_chars  = 0;

    newline_count = 0;

    if (posix_entries == NULL) run_scan();

    printf(ANSI_ERASE_ALL_AHEAD);

    // If enabled, print current directory name.

    if (!cfg_oneshot) {
        printf(ANSI_INVERT ANSI_BOLD "%s", current_dir);
        if (current_dir[0] != 0 && current_dir[1] != 0) putchar('/');

        printf(ANSI_RESET "\n");
        ++newline_count;
    }

#if DEBUG
    printf("Dev Build %s %s\n", __DATE__, __TIME__);
    ++newline_count;
#endif

    entry_row_offset = newline_count;

    printf(ANSI_RESET);

    if (entry_count < 0) {
        // The directory couldn't be opened.  Say so.
        printf(MSG_CANT_SCAN ANSI_RESET);
    } else if (entry_count == 0) {
        // The directory is empty.  Say so.
        printf(MSG_EMPTY ANSI_RESET);
    }

    if (total_length < termsize.ws_col) {
        // If we can fit on one line, no need to format.
        formatted = 0;
    } else {
        // Determine max number of columns using rightmost binary search.

        int lo = 1;
        int hi = entry_count - 1;

        while (lo < hi) {
            int m = lo + (hi - lo) / 2;
            if (valid_column_count(m, false)) lo = m + 1;
            else                              hi = m;
        }

        // entry_columns will be at least 1.
        entry_columns = (lo <= 1) ? 1 : lo - 1;
        entry_lines   = (entry_count - 1) / entry_columns + 1;

        entry_column_widths = realloc(entry_column_widths,
                                      sizeof(*entry_column_widths) * entry_columns);

        valid_column_count(entry_columns, true);
    }

    // If formatted, make sure we can fit all the rows.
    if (!cfg_oneshot && formatted && (entry_lines > termsize.ws_row)) {
        int page_length = (termsize.ws_row - entry_row_offset) * entry_columns;
        i_offset = selected / page_length * page_length;
        i_limit  = i_offset + page_length - 1;
    } else {
        i_offset = SELECTED_MIN;
        i_limit  = SELECTED_MAX;
    }

    for (int i = i_offset; i <= i_limit && i < entry_count; ++i) {
        if (formatted) {
            // If this entry would line wrap, print a newline.
            if (++next_column > entry_columns) {
                putchar('\n');
                next_column = 1;
                used_chars  = 0;
                ++newline_count;
            }
        }

        // If this is the currently selected entry,
        // copy the name into the selected name buffer and highlight it.
        if (!cfg_oneshot && i == selected) {
            memcpy(selected_name, posix_entries[i]->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);
            printf(ANSI_INVERT);
        }

        // Save cursor position for later use.

        entry_data[i].cells_over = used_chars;

        if (formatted) {
            entry_data[i].cells_down = newline_count;
            used_chars += write_entry(i, entry_column_widths[next_column - 1]);
        } else {
            entry_data[i].cells_down = entry_row_offset;
            used_chars += write_entry(i, entry_data[i].len);
        }
    }
}

static void refresh_entry(int index) {
    if (index == selected) printf(ANSI_INVERT);
    else                   printf(ANSI_RESET);

    printf(ANSI_CURSOR_LEFT ANSI_CURSOR_DOWN,
           termsize.ws_col, entry_data[index].cells_down);

    // Prevent terminals forcing at least 1 column forward.
    if (entry_data[index].cells_over > 0) {
        printf(ANSI_CURSOR_RIGHT, entry_data[index].cells_over);
    }

    write_entry(index, entry_data[index].len);

    // Restore cursor to previous row.
    printf(ANSI_CURSOR_UP, entry_data[index].cells_down);
}

static void refresh_display() {
    struct winsize new_termsize;

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &new_termsize);

    validate_selection_index();

    if (display_is_dirty
        || new_termsize.ws_row != termsize.ws_row
        || new_termsize.ws_col != termsize.ws_col) {
        // The terminal size changed
        // or the display info is incorrect
        // so we need to completely redraw.

        termsize = new_termsize;

        // Move to start of row, print, then move back to the original row.
        printf(ANSI_CURSOR_LEFT, termsize.ws_col);
        renew_display();
        printf(ANSI_CURSOR_UP, newline_count);

        display_is_dirty = false;
    } else {
        // Reflect changes in entry selection.

        if (entry_count >= 1) {
            memcpy(selected_name,
                   posix_entries[selected]->d_name,
                   sizeof(*selected_name) * SELECTED_MAXLEN);

            if (selected_previously > SELECTED_NOT) {
                refresh_entry(selected_previously);
            }

            refresh_entry(selected);
        }
    }

    // Update status bar.

    // But not if we're a oneshot.
    if (cfg_oneshot) return;

    printf(ANSI_CURSOR_LEFT ANSI_CURSOR_RIGHT ANSI_ERASE_TO_LINE_END,
           termsize.ws_col, utf8_len((unsigned char *)current_dir) + 1);

    // TODO: If this results in a line wrap,
    // we won't be on the line we think we're on.
    // Need to prevent/account for this.
    switch (prompt) {
    case PROMPT_ERR:
        printf("\e[31m"); // Foreground color red.
    case PROMPT_MSG:
        printf(ENTRY_DELIM "%s" ANSI_RESET, prompt_buffer);
        prompt = PROMPT_NONE;
        break;
    case PROMPT_CMD:
    case PROMPT_SEARCH:
        printf(ENTRY_DELIM);
        if (prompt == PROMPT_SEARCH) {
            putchar('/');
        } else {
            putchar(':');
        }
        printf("%s" ANSI_INVERT " " ANSI_RESET, prompt_buffer);
        break;
    default: break;
    }

    // This currently isn't necessary, but in case the cursor is showing
    // it would be nice to keep it at the top left of the display.
    printf(ANSI_CURSOR_LEFT, termsize.ws_col);
}

// The first string in argv must be exec.
// The last string in argv must be NULL.
static void fork_exec(char * exec, char ** argv, bool below_display) {
    pid_t pid;
    char * env_id_old;
    long   env_id_int;
    char   env_id_new[80];
    char * env_selected;

    // Setup normal terminal environment.
    restore_tcattr();

    if (below_display) {
        // Move cursor below the display.
        for (int l = 0; l <= newline_count; ++l) putchar('\n');
        printf("$ %s\n", exec);
    } else {
        // Clear the display.
        printf(ANSI_ERASE_ALL_AHEAD);
        fflush(stdout);
    }

    // Get the parent peek's ID, if it exists.
    // Malformed strings will "convert" to 0.
    // Increment it and convert back to an environment string for the fork.

    env_id_old = getenv(ENV_NAME_CHILD_ID);

    if (env_id_old == NULL) {
        strcpy(env_id_new, ENV_NAME_CHILD_ID "=1");
    } else {
        env_id_int = strtol(env_id_old, NULL, 10);
        if (env_id_int < 0)        env_id_int = 0;
        if (env_id_int < LONG_MAX) ++env_id_int;
        sprintf(env_id_new, ENV_NAME_CHILD_ID "=%ld", env_id_int);
    }

    // Write the selected file name to the environment variables.

    env_selected = malloc(sizeof(*env_selected)
                          * strlen(ENV_NAME_SELECTED) + 1 + strlen(selected_name) + 1);
    sprintf(env_selected, ENV_NAME_SELECTED "=%s", selected_name);

    // Time to fork.

    pid = fork();

    if (pid > 0) {
        wait(NULL);
    } else if (pid == 0) {
        putenv(env_id_new);
        putenv(env_selected);
        execvp(exec, argv);
        exit(1); // If we got here, execvp failed.
    }

    free(env_selected);

    replace_tcattr();
    display_is_dirty = true;
}

static void fork_exec_no_argv(char * exec, bool below_display) {
    char * argv[2] = {exec, NULL};
    fork_exec(exec, argv, below_display);
}

static void exec_selection() {
    char * argv[2] = {get_selected_fullpath(), NULL};
    if (access(argv[0], X_OK) == 0) {
        fork_exec(argv[0], argv, true);
    }
    free(argv[0]);
}

static void open_selection(char * opener) {
    char * argv[3] = {opener, get_selected_fullpath(), NULL};
    fork_exec(opener, argv, false);
    free(argv[1]);
}

static void handle_user_act(user_action act) {
    if (act >= USER_ACT_MV_UP && act <= USER_ACT_MV_RIGHT) {
        selected_previously = selected;
    }

    switch (act) {
    case USER_ACT_MV_UP:
        if (!formatted) break;
        if (selected - entry_columns < SELECTED_MIN) {
            // There is no entry above, so
            // move to the last row in the column.
            int offset = entry_columns * (entry_lines - 1);
            if (selected + offset > SELECTED_MAX) offset -= entry_columns;
            selected += offset;
        } else {
            selected -= entry_columns;
        }
        break;
    case USER_ACT_MV_DOWN:
        if (!formatted) break;
        if (selected + entry_columns > SELECTED_MAX) {
            // There is no entry below, so
            // move to the first row in the column.
            int offset = entry_columns * (entry_lines - 1);
            if (selected - offset < SELECTED_MIN) offset -= entry_columns;
            selected -= offset;
        } else {
            selected += entry_columns;
        }
        break;
    case USER_ACT_MV_LEFT:
        if (formatted) {
            // Check for cursor wrap by column.
            if (selected % entry_columns == 0) {
                selected += entry_columns - 1;
            } else {
                --selected;
            }
        } else {
            // Check for cursor wrap on a one-line display.
            if (selected - 1 < SELECTED_MIN) {
                selected = SELECTED_MAX;
            } else {
                --selected;
            }
        }
        break;
    case USER_ACT_MV_RIGHT:
        if (formatted) {
            // Check for cursor wrap by column.
            if (selected + 1 > SELECTED_MAX) {
                selected -= selected % entry_columns;
            } else if (selected % entry_columns == entry_columns - 1) {
                selected -= entry_columns - 1;
            } else {
                ++selected;
            }
        } else {
            // Check for cursor wrap on a one-line display.
            if (selected + 1 > SELECTED_MAX) {
                selected = SELECTED_MIN;
            } else {
                ++selected;
            }
        }
        break;
    case USER_ACT_CD_PARENT:
        cd("..");
        break;
    case USER_ACT_CD_SELECT:
        cd(selected_name);
        break;
    case USER_ACT_CD_RELOAD:
        free_posix_entries();
        break;
    case USER_ACT_ON_EDIT:
        open_selection(EXEC_NAME_EDITOR);
        break;
    case USER_ACT_ON_EXEC:
        exec_selection();
        break;
    case USER_ACT_ON_OPEN:
        open_selection(EXEC_NAME_OPENER);
        break;
    case USER_ACT_SEARCH:
        prompt = PROMPT_SEARCH;
        memset(prompt_buffer, 0, sizeof(prompt_buffer));
        prompt_buffer_i = 0;
        break;
    case USER_ACT_SHELL:
        fork_exec_no_argv(cfg_shell_path, true);
        break;
    }
}

int main(int argc, char ** argv) {
    int flag;
    char * start_dir = ".";

    setlocale(LC_ALL, "");

    while ((flag = getopt(argc, argv, SHORT_FLAGS)) != -1) { switch(flag) {
    case 'A':
    case 'a': cfg_show_dotfiles = 1; break;
    case 'B': cfg_color         = 0; break;
    case 'c': cfg_clear_trace   = 1; break;
    case 'F': cfg_indicate      = 1; break;
    case 'o': cfg_oneshot       = 1; break;
    case 'h': printf(MSG_HELP, argv[0]); return 0;
    case 'v': printf(MSG_VERSION); return 0;
    case '?': fprintf(stderr, MSG_INVALID, argv[0], argv[0]); return 1;
    default: abort();
    }}

    // If there is a remaining argument, it is the directory to start in.
    if (optind < argc) start_dir = argv[optind];

    cd(start_dir);
    if (prompt) goto quit;

    // Configure terminal to our needs.
    replace_tcattr();

    // Figure out what shell we're using.
    {
        char * env_shell = getenv("SHELL");

        if (env_shell) {
            cfg_shell_path = env_shell;
        }
    }

display_then_wait:
    refresh_display();

    if (cfg_oneshot) goto quit;

    // TODO: Most of the letter keybinds should be function keys.
    // Not all keyboards have these letters!

wait_for_user_act:
    if (prompt != PROMPT_CMD && prompt != PROMPT_SEARCH) {
        switch (getchar()) {
        default: goto wait_for_user_act;
        case 0: goto quit;
        case 0x08: // BACKSPACE
        case 0x7F: // DEL
            handle_user_act(USER_ACT_CD_PARENT);
            break;
        case 0x1B: // ESC
            // Eat escape sequence start, then match the code.
            getchar();
            switch (getchar()) {
            case '2': // Possible F9-F12.
                // F10 is "^[[21~".
                if (getchar() == '1' && getchar() == '~') {
                    goto quit;
                }
                break;
            case 'A': // Up Arrow
                handle_user_act(USER_ACT_MV_UP);
                break;
            case 'B': // Down Arrow
                handle_user_act(USER_ACT_MV_DOWN);
                break;
            case 'C': // Right Arrow
                handle_user_act(USER_ACT_MV_RIGHT);
                break;
            case 'D': // Left Arrow
                handle_user_act(USER_ACT_MV_LEFT);
                break;
            }
            break;
        case '\n':
            handle_user_act(USER_ACT_CD_SELECT);
            break;
        case '/':
            handle_user_act(USER_ACT_SEARCH);
            break;
        case 'E': case 'e':
            handle_user_act(USER_ACT_ON_EDIT);
            break;
        case 'H': case 'h':
            handle_user_act(USER_ACT_MV_LEFT);
            break;
        case 'J': case 'j':
            handle_user_act(USER_ACT_MV_DOWN);
            break;
        case 'K': case 'k':
            handle_user_act(USER_ACT_MV_UP);
            break;
        case 'L': case 'l':
            handle_user_act(USER_ACT_MV_RIGHT);
            break;
        case 'O': case 'o':
            handle_user_act(USER_ACT_ON_OPEN);
            break;
        case 'Q': case 'q':
            goto quit;
        case 'R': case 'r':
            handle_user_act(USER_ACT_CD_RELOAD);
            break;
        case 'S': case 's':
            handle_user_act(USER_ACT_SHELL);
            break;
        case 'X': case 'x':
            handle_user_act(USER_ACT_ON_EXEC);
            break;
        }
    } else {
        char c = getchar();

        switch (c) {
        case 0x08: // BACKSPACE
        case 0x7F: // DEL
            if (prompt_buffer_i > 0) {
                prompt_buffer[--prompt_buffer_i] = 0;
                perform_search();
            }
            break;
        case '\n':
            handle_user_act(USER_ACT_CD_SELECT);
        case 0x1B: // ESC
            prompt = PROMPT_NONE;
            break;
        default:
            if (prompt_buffer_i >= PROMPT_MAXLEN - 1) break;
            prompt_buffer[prompt_buffer_i++] = c;
            prompt_buffer[prompt_buffer_i] = 0;
            perform_search();
            break;
        }
    }

    goto display_then_wait;

quit:
    if (prompt) {
        printf("%s\n", prompt_buffer);
    }
    return 0;
}
