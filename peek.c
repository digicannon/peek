#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifndef DEBUG
#ifndef RELEASE
#define DEBUG 1
#else
#define DEBUG 0
#endif
#endif

#define COLOR_RESET  "\e[m"
#define COLOR_BOLD   "\e[1m"
#define COLOR_INVERT "\e[7m"

#define SHORT_FLAGS "aBcdFhx"
#define MSG_USAGE   "Usage: %s [-" SHORT_FLAGS "] [<directory>]"
#define MSG_INVALID MSG_USAGE "\nTry '%s -h' for more information.\n"
#define MSG_HELP MSG_USAGE "\nInteractive exploration of directories on the command line.\n"              \
                           "\nFlags:\n"                                                                   \
                           "  -a\tShow files starting with . (hidden by default)\n"                       \
                           "  -B\tDon't output color.\n"                                                  \
                           "  -c\tClear listing on exit.\n"                                               \
                           "  -d\tPrint current directory path before listing.\n"                         \
                           "  -F\tAppend ls style indicators to the end of entries.\n"                    \
                           "  -h\tPrint this message and exit.\n"                                         \
                           "  -x\tPrint unprintable characters as hex.  Carriage return would be /0D/.\n" \
                           "\nKeys:\n"                                                                    \
                           "   E\tEdit selected entry.\n"                                                 \
                           "   O\tOpen selected entry.\n"                                                 \
                           "   X\tExecute selected entry.\n"                                              \
                           "   Q\tQuit.\n"                                                                \
                           "   K|Up           Go up a directory.\n"                                       \
                           "   J|Down|Enter   Open selected directory.\n"                                 \
                           "   H|Left         Move selection left.\n"                                     \
                           "   L|Right        Move selection right.\n"
#define MSG_CANT_SCAN "could not scan"
#define MSG_EMPTY     "empty"

// 8.3 was FAT's max filename.  That sounds like a good minimum.
#define MIN_ENTRY_LEN 13
#define ENTRY_DELIM "  "
#define ENTRY_DELIM_LEN 2

// UTF8: If 8th bit is set, this code point is multiple bytes.
// If both the 8th and 7th bits are set, this byte is not the first byte.
// Therefore, only add to the print count if:
// 1) This code point is only 1 byte (8th bit not set).
// 2) The 8th bit is set but not the 7th.
// Simplified, the 8th and 7th bits can't be 1 and 0 respectively.
#define UTF8_COUNTABLE(c) ((c & 0xC0) != 0x80)
// A character is printable if above the controls and not DEL.
#define UTF8_PRINTABLE(c) (c > 0x1F && c != 0x7F)

#define OPEN_IN_PROCESS 0
#define OPEN_WITH_FORK  1

typedef struct peek_entry {
    int len; // Printed UTF8 length, not number of bytes.
    const char * color;
    char indicator;
} peek_entry;

static char * current_dir = NULL;
static int current_dir_len = 0;

static struct dirent ** posix_entries = NULL;
static peek_entry * entry_data = NULL;
static int entry_count = 0;   // Number of entries in current dir.

static int avg_columns   = 0; // Average output length of entries.
static int total_length  = 0; // Length of output without newlines.
static char formatted    = 1; // If set, output will do column formatting.
static int last_newline_count = 0; // Number of lines overflowed by last display.

#define SELECTED_MIN 0
#define SELECTED_MAX (entry_count - 1)
static int selected = SELECTED_MIN;
#define SELECTED_MAXLEN 256
static char selected_name[SELECTED_MAXLEN];

static struct termios tcattr_old;
static struct termios tcattr_raw;

static char cfg_show_dotfiles = 0; // (-a) If set, files starting with . will be shown.
static char cfg_color         = 1; // (-B) If set, color output.  -B unsets this.
static char cfg_clear_trace   = 0; // (-c) If set, clear displayed text on exit.
static char cfg_show_dir      = 0; // (-d) If set, print current dir before listing.
static char cfg_indicate      = 0; // (-F) If set, append indicators to entries.
static char cfg_format_hori   = 0; // (-h) If set, format horizontally.
static char cfg_print_hex     = 0; // (-x) If set, print unprintable characters as hex.

// TEMP:
#define CHECKBAD(val, err, msg, ...) if (val) { putchar('\n'); fprintf(stderr, msg, __VA_ARGS__); exit(err); }

static void restore_tcattr() {
    printf("\e[?25h"); // Show cursor.
    if (cfg_clear_trace) printf("\e[u\e[0J\e[2K"); // Clear last display, see display().
    else for (int l = 0; l <= last_newline_count; ++l) putchar('\n');

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

// If to, the current directory string being appended, is NULL,
// exact space is allocated and current_dir is copied.
// Returns to, whether that be the original or a result of the above condition.
// NOTE: If not NULL, to must be a copy of to.
static char * append_to_cd(char * to, char * suffix) {
    size_t suffix_len = strlen(suffix);
    size_t to_len = current_dir_len + suffix_len + 2;

    CHECKBAD(to_len > PATH_MAX, 1, "%s/%s is too long of a path!", to, suffix);
    if (to == NULL) to = malloc(sizeof(*current_dir) * to_len);
    CHECKBAD(to == NULL, 1, "Out of memory!%c", 0);

    memcpy(to, current_dir, sizeof(*current_dir) * current_dir_len);
    to[current_dir_len] = '/';
    memcpy(to + current_dir_len + 1, suffix, sizeof(*current_dir) * suffix_len);
    to[current_dir_len + 1 + suffix_len] = 0;

    return to;
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
        char * ent_path = append_to_cd(NULL, ent->d_name);
        if (access(ent_path, X_OK) == 0) {
            *color     = "\e[32;1m";
            *indicator = '*';
        } else {
            *color     = 0;
            *indicator = 0;
        }
        free(ent_path);
    }
}

static void run_scan() {
    int old_entry_count = entry_count;
    int longest_entry_len = 0;
    int len = 0;

    entry_count = scandir(current_dir, &posix_entries, display_filter, alphasort);
    if (entry_count == -1) {
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

// TODO: Replace realpath.  We shouldn't be resolving symlinks.
static char cd(char * to) {
    char * old_path;
    int old_len = current_dir_len;

    if (current_dir) {
        // Only provide fallback if this is not the initial path.
        old_path = malloc(sizeof(*old_path) * PATH_MAX);
        memcpy(old_path, current_dir, sizeof(*old_path) * PATH_MAX);
    }

    if (current_dir == NULL) {
        current_dir = malloc(sizeof(*current_dir) * PATH_MAX);
        realpath(to, current_dir);
    } else if (to[0] == '/') {
        realpath(to, current_dir);
    } else {
        char * new = append_to_cd(NULL, to);
        realpath(new, current_dir);
        free(new);
    }

    current_dir_len = strlen(current_dir);

    if (posix_entries) {
        for (int i = 0; i < entry_count; ++i) free(posix_entries[i]);
        free(posix_entries);
        posix_entries = NULL;
    }

    run_scan();
    if (entry_count == -1) {
        // This "directory" (could be a file or something) failed to scan.
        // Restore old file path.
        free(current_dir);
        current_dir     = old_path;
        current_dir_len = old_len;
        return 0;
    }
    free(old_path);

    selected = SELECTED_MIN;
    return 1;
}

// NOTE: This will eat everything in stdin.
static void get_cursor_pos(int * row, int * col) {
    int ahead = 0;
    char c;

    if (row) *row = 0;
    if (col) *col = 0;

    // Attempt to clear out stdin.
    // CLEANUP: Is read with a NULL buffer really allowed?
    ioctl(STDIN_FILENO, FIONREAD, &ahead);
    read(STDIN_FILENO, NULL, ahead);

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

static void display() {
    struct winsize termsize;
    int newline_count = 0; // Amount of newlines printed.
    int next_column   = 0;
    int row_before, col_before;
    int row_after,  col_after;
    struct dirent * d_child = NULL;
    const char * d_child_color;
    char d_child_indicator;
    int used_chars = 0;

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &termsize);

    if (posix_entries == NULL) run_scan();

    // Validate selection index.

    if (entry_count < 1) selected = 0;
    else if (selected < SELECTED_MIN) selected = SELECTED_MAX;
    else if (selected > SELECTED_MAX) selected = SELECTED_MIN;

    // Return to start of last display and erase previous.
    // 0J erases below cursor, 2K erases to the right.

    printf("\e[u\e[0J\e[2K");
    get_cursor_pos(&row_before, &col_before);

    // If enabled, print current directory name.

    if (cfg_show_dir) {
        printf(COLOR_INVERT COLOR_BOLD "%s" COLOR_RESET "\n", current_dir);
        ++newline_count;
    }

    printf(COLOR_RESET);

    if (entry_count < 0) {
        // The directory couldn't be opened.  Say so.
        printf(MSG_CANT_SCAN COLOR_RESET);
    } else if (entry_count == 0) {
        // The directory is empty.  Say so.
        printf(MSG_EMPTY COLOR_RESET);
    }

    // If we can fit on one line, no need to format.
    if (total_length < termsize.ws_col) {
        formatted = 0;
    }

    for (int i = 0; i < entry_count; ++i) {
        d_child = posix_entries[i];
        d_child_color = entry_data[i].color;
        d_child_indicator = entry_data[i].indicator;
        used_chars = 0;

        if (formatted) {
            // If we're putting entries in columns,
            // we need to figure out where this entry should end.
            // If it would line wrap, print a newline.

            int intended_end = avg_columns + ENTRY_DELIM_LEN;
            intended_end *= ++next_column;
            if (cfg_indicate) ++intended_end;

            if (intended_end >= termsize.ws_col) {
                putchar('\n');
                next_column = 1;
                ++newline_count;
            }
        }

        if (i == selected) {
            // Copy the name into the selected_name buffer for possible cd request.
            memcpy(selected_name, d_child->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);
            printf(COLOR_INVERT);
        }

        // If enabled, print the corresponding color for the type.
        if (d_child_color) printf("%s", d_child_color);

        // Print the name of the entry.
        for (unsigned char * c = (unsigned char *)d_child->d_name; *c; ++c) {
            if (formatted) {
                // Stop early for end of column.

                int len = utf8_char_len(*c);
                used_chars += len;

                if (used_chars >= (d_child_indicator ? avg_columns - 1 : avg_columns)) {
                    // Replace last character with truncation indictaor.
                    printf(COLOR_RESET "~");
                    if (len == 3) used_chars -= 2;
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

        printf(COLOR_RESET);

        // If enabled, print the corresponding indicator for the type.
        if (d_child_indicator) {
            putchar(d_child_indicator);
            ++used_chars;
        }

        if (formatted) {
            for (; used_chars < avg_columns; ++used_chars) putchar(' ');
        }
        printf(ENTRY_DELIM);
    }

    // If the lines overflowed does not match the difference in cursor height,
    // the terminal scrolled and we need to adjust the saved position.

    get_cursor_pos(&row_after, &col_after);
    last_newline_count = newline_count;
    if (newline_count) {
        // Move cursor up to adjust for overflow and save it.
        printf("\e[%d;%df\e[s", row_after - newline_count, col_before);
    }
}

static int open_selection(char * opener, int do_fork) {
    char * argv[3] = {0};
    char * selected_path;
    pid_t pid;

    selected_path = append_to_cd(NULL, selected_name);

    // If opener is null, then we are executing the selection, not "opening" it.
    if (opener == NULL) {
        opener  = selected_path;
        argv[0] = selected_path;
    } else {
        argv[0] = opener;
        argv[1] = selected_path;
    }

    if (do_fork) {
        pid = fork();

        if (pid > 0) {
            return 0;
        } else if (pid == 0) {
            execv(opener, argv);
            CHECKBAD(1, 1, "%s failed to execute", opener);
        } else {
            CHECKBAD(1, 1, "Could not start process for %s", opener);
        }
    } else {
        restore_tcattr(); // atexit won't call this since we are overwriting this process.
        if (cfg_clear_trace) putchar('\n'); // execv might stdout buffered before clear happens.
        execv(opener, argv);
        CHECKBAD(1, 1, "%s failed to execute", opener);
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
    case 'd': cfg_show_dir      = 1; break;
    case 'F': cfg_indicate      = 1; break;
    case 'x': cfg_print_hex     = 1; break;
    case 'h': printf(MSG_HELP, argv[0]); return 0;
    case '?': fprintf(stderr, MSG_INVALID, argv[0], argv[0]); return 1;
    default: abort();
    }}

    // If there is a remaining argument, it is the directory to start in.
    if (optind < argc) start_dir = argv[optind];

    // Make sure the starting directory is valid.  Fallbacks need it to be.
    if (!cd(start_dir)) {
        fprintf(stderr, "%s\n", strerror(errno));
        return 1;
    }

    // Create raw terminal mode to stop stdin buffer from breaking key press detection.
    // http://pubs.opengroup.org/onlinepubs/000095399/basedefs/termios.h.html#tag_13_74_03_06
    tcgetattr(STDIN_FILENO, &tcattr_old);
    atexit(restore_tcattr); // Restore old mode when we're done.
    memcpy(&tcattr_raw, &tcattr_old, sizeof(struct termios));
    tcattr_raw.c_cc[VMIN]  = 0;
    tcattr_raw.c_cc[VTIME] = 0;
    tcattr_raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_raw);
#if DEBUG
    printf("Dev Build %s %s\e[K\n", __DATE__, __TIME__);
#endif
    printf("\e[?25l\e[s"); // Hide cursor and save cursor location.

    display();
redo:
    switch (getchar()) {
    default: goto redo;
    case 'E': case 'e': // Edit
        return open_selection("/usr/bin/vim", OPEN_IN_PROCESS);
    case 'O': case 'o': // Open
        return open_selection("/usr/bin/xdg-open", OPEN_WITH_FORK);
    case 'X': case 'x': // eXecute
        return open_selection(NULL, OPEN_IN_PROCESS);
    case 'K': case 'k': // Select Parent
        cd(".."); break;
    case 'J': case 'j': // Select
    case '\n':
        cd(selected_name); break;
    case 'H': case 'h': // Left
        --selected; break;
    case 'L': case 'l': // Right
        ++selected; break;
    case 'Q': case 'q': // Quit
        return 0;
    case 0x1B: // ESC
        if (getchar() != '[') return 0; // If just escape key, quit.

        // Arrow key escape codes have to be handled seperately than
        // the above handling of ASCII keys.
        switch (getchar()) {
        case 'A': // Select Parent (Up)
            cd(".."); break;
        case 'B': // Select (Down)
            cd(selected_name); break;
        case 'D': // Left
            --selected; break;
        case 'C': // Right
            ++selected; break;
        }
        break;
    }

    display();
    goto redo;
}
