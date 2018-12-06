#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define COLOR_D_NAME "\e[1m\e[7m"
#define COLOR_NORMAL "\e[m"
#define COLOR_NORMAL_INVALID "\e[m\e[2m"
#define COLOR_SELECT "\e[7m"
#define COLOR_SELECT_INVALID "\e[7m\e[2m"

#define MSG_EMPTY "empty"

#define OPEN_IN_PROCESS 0
#define OPEN_WITH_FORK  1

static char * d_current_name = NULL;
static int d_current_len = 0; // Strlen of current dir name.
static int d_length = 0;      // Number of entries in current dir.

#define SELECTED_MIN 0
#define SELECTED_MAX (d_length - 1)
static int selected = SELECTED_MIN;
#define SELECTED_MAXLEN 256
static char selected_name[SELECTED_MAXLEN];
static char selected_valid = 0;

static struct termios tcattr_old;
static struct termios tcattr_raw;

typedef char togglable;
static togglable cfg_clear_trace = 1; // If unset, clear displayed text on exit.
static togglable cfg_show_dir = 1; // Print current dir.
static togglable cfg_show_dotfiles = 1; // If unset, file starting with . won't be shown.

#define CHECKBAD(val, err, msg, ...) if (val) { putchar('\n'); fprintf(stderr, msg, __VA_ARGS__); exit(err); }

static void restore_tcattr_old() {
    printf("\e[?25h"); // Show cursor.
    if (cfg_clear_trace) printf("\e[u\e[0J\e[2K"); // Clear last display, see display().
    else                 putchar('\n');

    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_old);
}

// If to, the current directory string being appended, is NULL,
// exact space is allocated and d_current_name is copied.
// Returns to, whether that be the original or a result of the above condition.
static char * append_to_cd(char * to, char * suffix) {
    size_t suffix_len = strlen(suffix);
    size_t to_len = d_current_len + suffix_len + 2;

    CHECKBAD(to_len > PATH_MAX, 1, "%s/%s is too long of a path!", to, suffix);
    if (to == NULL) to = malloc(sizeof(*d_current_name) * to_len);
    CHECKBAD(to == NULL, 1, "Out of memory!%c", 0);

    memcpy(to, d_current_name, sizeof(*d_current_name) * d_current_len);
    to[d_current_len] = '/';
    memcpy(to + d_current_len + 1, suffix, sizeof(*d_current_name) * suffix_len);
    to[d_current_len + 1 + suffix_len] = 0;

    return to;
}

static void cd(char * to) {
    if (d_current_name == NULL) {
        d_current_name = malloc(sizeof(*d_current_name) * PATH_MAX);
        realpath(to, d_current_name);
    } else if (to[0] == '/') {
        realpath(to, d_current_name);
    } else {
        char * new = append_to_cd(NULL, to);
        realpath(new, d_current_name);
        free(new);
    }

    d_current_len = strlen(d_current_name);
    d_length = 0;
    selected = SELECTED_MIN;
}

static void check_entry_selectable(struct dirent * ent) {
    struct stat ent_stat;
    char * ent_path;

    ent_path = append_to_cd(NULL, ent->d_name);
    CHECKBAD(lstat(ent_path, &ent_stat), 1, "Could not lstat %s", ent->d_name);
    free(ent_path);

    // TODO: Readable check was wrong.  Do more research.
    selected_valid =
        //(ent_stat.st_mode & S_IRGRP) && // Readable by user group?
        S_ISDIR(ent_stat.st_mode);      // Is it a directory?
}

static int display_filter(const struct dirent * ent) {
    if (ent->d_name[0] == '.') {
	if (!cfg_show_dotfiles) return 0;
	else if (ent->d_name[1] == 0) return 0; // Don't show "."
	else if (ent->d_name[1] == '.' && ent->d_name[2] == 0) return 0; // Don't show ".."
    }
    return 1;
}

static void display() {
    static int last_line_len = 0;

    DIR * d_current;
    int d_current_fd;
    struct dirent ** d_children;
    struct dirent * d_child;

    d_length = scandir(d_current_name, &d_children, display_filter, alphasort);
    CHECKBAD(d_length == -1, 1, "Could not scan %s", d_current_name);
    // TODO: Just don't open if this happens.

    d_current = opendir(d_current_name);
    CHECKBAD(d_current == NULL, 1, "Could not open %s", d_current_name);
    d_current_fd = dirfd(d_current);

    // Validate selection index.

    if (d_length < 1) selected = 0;
    else if (selected < SELECTED_MIN) selected = SELECTED_MAX;
    else if (selected > SELECTED_MAX) selected = SELECTED_MIN;

    // Return to start of last display and erase previous.
    // 0J erases below cursor, 2K erases to the right.

    printf("\e[u\e[0J\e[2K");

    // If enabled, print current directory.

    if (cfg_show_dir) {
        printf(COLOR_D_NAME "%s" COLOR_NORMAL ": ", d_current_name);
        last_line_len = d_current_len + 2;
    }

    // Now we can print the names of each entry.

    if (d_length <= 0) {
        last_line_len += printf(COLOR_NORMAL MSG_EMPTY COLOR_NORMAL " ");
    }

    for (int i = 0; i < d_length; ++i) {
        d_child = d_children[i];

        if (i == selected) {
	    // The selection is valid if it is a directory.
	    // Print the selection with proper coloring, then
	    // copy the name into the selected_name buffer.

            //check_entry_selectable(d_child);
	    selected_valid = d_child->d_type == DT_DIR;
            last_line_len += printf("%s%s" COLOR_NORMAL " ",
                    selected_valid ? COLOR_SELECT : COLOR_SELECT_INVALID,
                    d_child->d_name);
            memcpy(selected_name, d_child->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);
        } else {
	    // Just print the name with the color depending on if it is a directory.
            last_line_len += printf("%s%s" COLOR_NORMAL " ",
                    d_child->d_type == DT_DIR ? COLOR_NORMAL : COLOR_NORMAL_INVALID,
                    d_child->d_name);
        }

        free(d_child);
    }

    free(d_children);
    closedir(d_current);
}

int open_selection(char * opener, int do_fork) {
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
	printf("we're forking!");
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
	restore_tcattr_old(); // atexit won't call this since we are overwriting this process.
	if (cfg_clear_trace) putchar('\n'); // execv might stdout buffered before clear happens.
	execv(opener, argv);
	CHECKBAD(1, 1, "%s failed to execute", opener);
    }
}

int main(int argc, char ** argv) {
    setlocale(LC_ALL, "");

    // TODO: Read flags.
    // TEMP: Usage: peek [dir]
    if (argc == 1) cd(".");
    else cd(argv[1]);

    // Create raw terminal mode to stop stdin buffer from breaking key press detection.
    // http://pubs.opengroup.org/onlinepubs/000095399/basedefs/termios.h.html#tag_13_74_03_06
    tcgetattr(STDIN_FILENO, &tcattr_old);
    atexit(restore_tcattr_old); // Restore old mode when we're done.
    memcpy(&tcattr_raw, &tcattr_old, sizeof(struct termios));
    tcattr_raw.c_cc[VMIN]  = 0;
    tcattr_raw.c_cc[VTIME] = 0;
    tcattr_raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_raw);
    printf("\e[2J\e[?25l\e[s"); // Hide cursor and save cursor location.

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
    case 'K': case 'k': // Back
         cd(".."); break;
    case 'J': case 'j': // Select
    case '\n':
         if (selected_valid) cd(selected_name);
         break;
    case 'H': case 'h': // Left
         --selected; break;
    case 'L': case 'l': // Right
         ++selected; break;
    case 'Q': case 'q': // Quit
         return 0;
    case 27: // ESC
         if (getchar() != '[') return 0; // If just escape key, quit.

         // Arrow key escape codes have to be handled seperately than
         // the above handling of ASCII keys.
         switch (getchar()) {
         case 'A': // Back (Up)
             cd(".."); break;
         case 'B': // Select (Down)
             if (selected_valid) cd(selected_name);
             break;
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
