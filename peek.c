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

#define ANSI_RESET  "\e[m"
#define ANSI_BOLD   "\e[1m"
#define ANSI_INVERT "\e[7m"

#define ANSI_SHOW_CURSOR "\e[?25h"
#define ANSI_HIDE_CURSOR "\e[?25l"

#define SHORT_FLAGS "aBcdFhx"
#define MSG_USAGE   "Usage: %s [-" SHORT_FLAGS "] [<directory>]"
#define MSG_INVALID MSG_USAGE "\nTry '%s -h' for more information.\n"
#define MSG_HELP MSG_USAGE "\nInteractive exploration of directories on the command line.\n"              \
                           "\nFlags:\n"                                                                   \
                           "  -a\tShow files starting with . (hidden by default).\n"                      \
                           "  -B\tDon't output color.\n"                                                  \
                           "  -c\tClear listing on exit.\n"                                               \
                           "  -d\tPrint current directory path before listing.\n"                         \
                           "  -F\tAppend ls style indicators to the end of entries.\n"                    \
                           "  -h\tPrint this message and exit.\n"                                         \
                           "  -x\tPrint unprintable characters as hex.  Carriage return would be \\0D.\n" \
                           "\nKeys:\n"                                                                    \
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
                           "   X\tExecute selected entry.\n"
#define MSG_CANT_SCAN "could not scan"
#define MSG_EMPTY     "empty"

// 8.3 was FAT's max filename.  That sounds like a good minimum.
#define MIN_ENTRY_LEN   13
#define ENTRY_DELIM     "  "
#define ENTRY_DELIM_LEN 2

// UTF8: If 8th bit is set, this code point is multiple bytes.
// If both the 8th and 7th bits are set, this byte is not the first byte.
// Therefore, only add to the print count if:
// 1) This code point is only 1 byte (8th bit not set).
// 2) The 8th bit is set but not the 7th.
#define UTF8_COUNTABLE(c) ((c & 0xC0) != 0x80)
// A character is printable if above the controls and not DEL.
#define UTF8_PRINTABLE(c) (c > 0x1F && c != 0x7F)

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
} user_action;

typedef struct termpos {
    int row;
    int col;
} termpos;

typedef struct peek_entry {
    int len; // Printed UTF8 length, not number of bytes.
    const char * color;
    char indicator;
    int row;
    int col;
} peek_entry;

enum prompt_t {
    PROMPT_NONE,
    PROMPT_ERR,
    PROMPT_MSG,
    PROMPT_FOR,
} prompt = PROMPT_NONE;

static char * current_dir     = NULL;
static size_t current_dir_len = 0;

static struct dirent ** posix_entries = NULL;
static peek_entry *     entry_data    = NULL;
static int              entry_count   = 0; // Number of entries in current dir.

static bool    display_is_dirty = true; // Force display redraw when true.
static termpos pos_status_bar;          // Column is the start of the selection name.
static int     entry_row_offset = 0;

static bool formatted;     // If true, output will do column formatting.
static int  avg_columns;   // Average output length of entries.
static int  total_length;  // Length of output without newlines.
static int  max_column;    // Number of format columns printed by last display.
static int  newline_count; // Number of lines printed by last display.
static int  entry_lines;   // Number of lines taken by entries, printed or not.

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

static struct termios tcattr_old;
static struct termios tcattr_raw;
static struct winsize termsize;

static bool cfg_show_dotfiles = 0; //  (-a) If set, files starting with . will be shown.
static bool cfg_color         = 1; // !(-B) If set, color output.
static bool cfg_clear_trace   = 0; //  (-c) If set, clear displayed text on exit.
static bool cfg_show_dir      = 1; // !(-d) If set, print current dir before listing.
static bool cfg_indicate      = 0; //  (-F) If set, append indicators to entries.
static bool cfg_format_hori   = 0; //  (-H) If set, format horizontally.
static bool cfg_print_hex     = 0; //  (-x) If set, print unprintable characters as hex.

static void restore_tcattr() {
    printf(ANSI_SHOW_CURSOR);
    if (cfg_clear_trace) printf("\e[0J\e[2K"); // Clear ahead and below.
    else for (int l = 0; l <= newline_count; ++l) putchar('\n');

    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_old);
}

static int display_filter(const struct dirent * ent) {
    if (ent->d_name[0] == '.') {
        if (!cfg_show_dotfiles) return 0;
        else if (ent->d_name[1] == 0) return 0; // Don't show "."
        else if (ent->d_name[1] == '.' && ent->d_name[2] == 0) return 0; // Don't show ".."
    }
    return 1;
}

static int utf8_char_len(unsigned char c) {
    if (UTF8_PRINTABLE(c)) {
        if (UTF8_COUNTABLE(c)) return 1;
    } else if (cfg_print_hex) {
        // 3 = \XX
        if (UTF8_COUNTABLE(c)) return 3;
    }
    return 0;
}

static int utf8_len(unsigned char * str) {
    int len = 0;
    for (unsigned char * c = str; *c; ++c) {
        len += utf8_char_len(*c);
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
    int longest_entry_len = 0;
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

    avg_columns  = 0;
    total_length = 0;
    formatted    = 1;

    // Calculate average display length and total length of output.

    for (int i = 0; i < entry_count; ++i) {
        entry_data[i].len = utf8_len((unsigned char *)posix_entries[i]->d_name);
        len = entry_data[i].len;

        get_entry_type(posix_entries[i], &entry_data[i].color, &entry_data[i].indicator);
        if (!cfg_color)    entry_data[i].color     = 0;
        if (!cfg_indicate) entry_data[i].indicator = 0;

        // Try to prevent abnormally sized entries from skewing average.
        if (i == 0 || (
                len < avg_columns / i + MIN_ENTRY_LEN
                && len >= MIN_ENTRY_LEN)) {
            avg_columns += len;
        }

        if (len > longest_entry_len) longest_entry_len = len;

        total_length += len;
        if (entry_data[i].indicator) ++total_length;
        total_length += ENTRY_DELIM_LEN;
    }

    if (entry_count) avg_columns /= entry_count;

    // Don't force columns to be bigger than the longest entry.
    if (avg_columns < MIN_ENTRY_LEN) {
        avg_columns = longest_entry_len < 13 ? longest_entry_len : 13;
    }
}

static void free_posix_entries() {
    if (posix_entries) {
        for (int i = 0; i < entry_count; ++i) free(posix_entries[i]);
        free(posix_entries);
        posix_entries = NULL;
    }
}

static void cd(char * to) {
    if (!sturdy_chdir(to)) {
        sprintf(prompt_buffer, "%s", strerror(errno));
        return;
    }

    if (current_dir) free(current_dir);

    if ((current_dir = sturdy_getcwd()) == NULL) {
        // TODO: This is fatal.  Do something to communicate.
        exit(1);
    }

    current_dir_len = strlen(current_dir);

    free_posix_entries();
    run_scan();

    selected            = SELECTED_MIN;
    selected_previously = SELECTED_NOT;
}

static int get_stdin_chars_ahead() {
    int ahead;
    ioctl(STDIN_FILENO, FIONREAD, &ahead);
    return ahead;
}

// NOTE: This will eat everything in stdin.
static void get_cursor_pos(int * row, int * col) {
    char c;

    if (row) *row = 0;
    if (col) *col = 0;

    // Attempt to clear out stdin.
    // CLEANUP: Is read with a NULL buffer really allowed?
    read(STDIN_FILENO, NULL, get_stdin_chars_ahead());

    // Request cursor position and scan for the response "\e[%d;%dR".
    // If we read in something that started out correct and became malformed,
    // it isn't the cursor position response so start over.
    printf("\e[6n");
scan_for_esc:
    while (getchar() != 0x1B);               // Scan for escape.
    if (getchar() != '[') goto scan_for_esc; // Scan for [
    while (1) {                              // Scan for %d;
        c = getchar();
        if (c == ';') break;
        else if (c < '0' || c > '9') goto scan_for_esc;
        if (row) *row = (*row * 10) + (c - '0');
    }
    while (1) {                              // Scan for %dR
        c = getchar();
        if (c == 'R') break;
        else if (c < '0' || c > '9') goto scan_for_esc;
        if (col) *col = (*col * 10) + (c - '0');
    }
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

static int write_entry(int index) {
    struct dirent * d_child           = posix_entries[index];
    const char *    d_child_color     = entry_data[index].color;
    char            d_child_indicator = entry_data[index].indicator;

    int used_chars = 0;
    int char_len;

    // If enabled, print the corresponding color for the type.
    if (d_child_color) printf("%s", d_child_color);
    
    // Print the name of the entry.
    for (unsigned char * c = (unsigned char *)d_child->d_name; *c; ++c) {
        char_len = utf8_char_len(*c);
        used_chars += char_len;
        
        if (formatted) {
            // Stop early for end of column.

            if (used_chars >= (d_child_indicator ? avg_columns - 1 : avg_columns)) {
                // Replace last character with truncation indictaor.
                printf(ANSI_RESET "~");
                if (char_len == 3) used_chars -= 2;
                break;
            }
        }

        // This character is printable if
        // it is above control characters and not DEL.
        if (UTF8_PRINTABLE(*c)) {
            putchar(*c);
        } else if (cfg_print_hex) {
            printf("\\%02X", (unsigned char)*c);
        }
    }

    printf(ANSI_RESET);

    // If enabled, print the corresponding indicator for the type.
    if (d_child_indicator) {
        putchar(d_child_indicator);
        ++used_chars;
    }

    if (formatted) {
        for (; used_chars < avg_columns; ++used_chars) putchar(' ');
    }
    used_chars += printf(ENTRY_DELIM);

    return used_chars;
}

static void renew_display() {
    // If formatting, this will be the next format column to use.
    // If not, this will be the amount of characters printed so far.
    //                          (literally the next terminal column)
    int next_column = 0;

    newline_count = 0;

    if (posix_entries == NULL) run_scan();

    // Return to start of last display and erase previous.
    // 0J erases below cursor, 2K erases to the right.

    printf("\e[0J\e[2K");

    // If enabled, print current directory name.

    if (cfg_show_dir) {
        printf(ANSI_INVERT ANSI_BOLD "%s", current_dir);
        if (current_dir[0] != 0 && current_dir[1] != 0) putchar('/');
    }

    get_cursor_pos(&pos_status_bar.row, &pos_status_bar.col);
    printf(ANSI_RESET "\n");
    ++newline_count;

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

    // If we can fit on one line, no need to format.
    if (total_length < termsize.ws_col) {
        formatted = 0;
    }

    // Calculate how many columns we have.
    max_column = avg_columns + ENTRY_DELIM_LEN;
    if (cfg_indicate) ++max_column;
    max_column = termsize.ws_col / max_column;
    
    // If formatted, make sure we can fit all the rows.
    if (formatted && (entry_count / max_column > termsize.ws_row)) {
        int page_length = (termsize.ws_row - entry_row_offset) * max_column;
        i_offset = selected / page_length * page_length;
        i_limit  = i_offset + page_length - 1;
    } else {
        i_offset = SELECTED_MIN;
        i_limit  = SELECTED_MAX;
    }

    for (int i = i_offset; i <= i_limit && i < entry_count; ++i) {
        if (formatted) {
            // If this entry would line wrap, print a newline.
            if (++next_column > max_column) {
                putchar('\n');
                next_column = 1;
                ++newline_count;
            }
        }

        // If this is the currently selected entry,
        // copy the name into the selected name buffer and highlight it.
        if (i == selected) {
            memcpy(selected_name, posix_entries[i]->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);
            printf(ANSI_INVERT);
        }

        // Save cursor position for later use.

        if (formatted) {
            entry_data[i].row = (i - i_offset) / max_column + entry_row_offset;
            entry_data[i].col = (i - i_offset) % max_column * (avg_columns + ENTRY_DELIM_LEN) + 1;
            write_entry(i);
        } else {
            entry_data[i].row = entry_row_offset;
            entry_data[i].col = next_column + 1;
            next_column += write_entry(i);
        }
    }

    if (newline_count) {
        // The terminal may have scrolled and we need to adjust the saved position.
        // Move cursor up to adjust for possible overflow and save it.
        int row_after;
        get_cursor_pos(&row_after, NULL);
        pos_status_bar.row = row_after - newline_count;
    }

    // Save number of lines taken by entries.  Possibly different from newline_count.
    entry_lines = entry_count / max_column * max_column;
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
        renew_display();
        display_is_dirty = false;
    } else {
        // Reflect changes in entry selection.

        if (entry_count >= 1) {
            memcpy(selected_name, posix_entries[selected]->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);

            if (selected_previously > SELECTED_NOT) {
                printf("\e[%d;%df" ANSI_RESET,
                        entry_data[selected_previously].row + pos_status_bar.row,
                        entry_data[selected_previously].col);
                write_entry(selected_previously);
            }

            printf("\e[%d;%df" ANSI_INVERT,
                   entry_data[selected].row + pos_status_bar.row,
                   entry_data[selected].col);
            write_entry(selected);
        }
    }

    // Update status bar.

    printf("\e[%d;%df\e[0K", pos_status_bar.row, pos_status_bar.col);
    printf(ANSI_BOLD "%s" ANSI_RESET, selected_name);

    switch (prompt) {
    case PROMPT_ERR:
        printf("\e[31m"); // Foreground color red.
    case PROMPT_MSG:
        printf(ENTRY_DELIM "%s" ANSI_RESET, prompt_buffer);
        prompt = PROMPT_NONE;
        break;
    case PROMPT_FOR:
        printf(ENTRY_DELIM ":%s", prompt_buffer);
        break;
    default: break;
    }

    // Return to starting row for next display.

    printf("\e[%d;%df", pos_status_bar.row, 0);
}

static void open_selection(char * opener) {
    char * argv[3] = {0};
    char * path    = selected_name;
    pid_t pid;

    // If opener is null, then we are executing the selection, not "opening" it.
    if (opener == NULL) {
        opener  = path;
        argv[0] = path;
    } else {
        argv[0] = opener;
        argv[1] = path;
    }

    pid = fork();

    if (pid > 0) {
        wait(NULL);
    } else if (pid == 0) {
        execvp(opener, argv);
        // If we got here, execvp failed.
        exit(1);
    }

    // Make sure our terminal attributes are still active.
    printf(ANSI_HIDE_CURSOR);
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_raw);
}

static void handle_user_act(user_action act) {
    if (act >= USER_ACT_MV_UP && act <= USER_ACT_MV_RIGHT) {
        selected_previously = selected;
    }

    switch (act) {
    case USER_ACT_MV_UP:
        if (!formatted) break;
        if (selected - max_column < SELECTED_MIN) {
            // There is no entry above, so
            // move to the last row in the column.
            int offset = max_column * (entry_lines - 1);
            if (selected + offset > SELECTED_MAX) offset -= max_column;
            selected += offset;
        } else {
            selected -= max_column;
        }
        break;
    case USER_ACT_MV_DOWN:
        if (!formatted) break;
        if (selected + max_column > SELECTED_MAX) {
            // There is no entry below, so
            // move to the first row in the column.
            int offset = max_column * (entry_lines - 1);
            if (selected - offset < SELECTED_MIN) offset -= max_column;
            selected -= offset;
        } else {
            selected += max_column;
        }
        break;
    case USER_ACT_MV_LEFT:
        if (formatted) {
            // Check for cursor wrap by column.
            if (selected % max_column == 0) {
                selected += max_column - 1;
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
                selected -= selected % max_column;
            } else if (selected % max_column == max_column - 1) {
                selected -= max_column - 1;
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
        open_selection(NULL);
        break;
    case USER_ACT_ON_OPEN:
        open_selection(EXEC_NAME_OPENER);
        break;
    }
}

int main(int argc, char ** argv) {
    int flag;
    char * start_dir = ".";

    setlocale(LC_ALL, "");

    while ((flag = getopt(argc, argv, SHORT_FLAGS)) != -1) { switch(flag) {
    case 'a': cfg_show_dotfiles = 1; break;
    case 'B': cfg_color         = 0; break;
    case 'c': cfg_clear_trace   = 1; break;
    case 'd': cfg_show_dir      = 0; break;
    case 'F': cfg_indicate      = 1; break;
    case 'x': cfg_print_hex     = 1; break;
    case 'h': printf(MSG_HELP, argv[0]); return 0;
    case '?': fprintf(stderr, MSG_INVALID, argv[0], argv[0]); return 1;
    default: abort();
    }}

    // If there is a remaining argument, it is the directory to start in.
    if (optind < argc) start_dir = argv[optind];

    cd(start_dir);

    // Create raw terminal mode to stop stdin buffer from breaking key press detection.
    // http://pubs.opengroup.org/onlinepubs/000095399/basedefs/termios.h.html#tag_13_74_03_06
    tcgetattr(STDIN_FILENO, &tcattr_old);
    atexit(restore_tcattr); // Restore old mode when we're done.
    memcpy(&tcattr_raw, &tcattr_old, sizeof(struct termios));
    tcattr_raw.c_cc[VMIN]  = 1;
    tcattr_raw.c_cc[VTIME] = 0;
    tcattr_raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_raw);
    printf(ANSI_HIDE_CURSOR);

display_then_wait:
    refresh_display();

wait_for_user_act:
    switch (getchar()) {
    default: goto wait_for_user_act;
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
    case 'X': case 'x':
        handle_user_act(USER_ACT_ON_EXEC);
        break;
    }

    goto display_then_wait;

quit:
    return 0;
}
