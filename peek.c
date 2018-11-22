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

static char cfg_show_dir = 1; // Print current dir.

#define CHECKBAD(val, err, msg, ...) if (val) { putchar('\n'); fprintf(stderr, msg, __VA_ARGS__); exit(err); }

static void restore_tcattr_old() {
    printf("\e[?25h\n"); // Show cursor & newline for end of output.
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
    if (ent->d_name[0] == '.') return 0;
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

    d_current = opendir(d_current_name);
    CHECKBAD(d_current == NULL, 1, "Could not open %s", d_current_name);
    d_current_fd = dirfd(d_current);

    // Erase last display call.

    if (last_line_len == 0) printf("\e[s"); // Save start of output for overwrite later.
    else                    printf("\e[u"); // Return to start of last display.

    for (int i = 0; i < last_line_len; ++i) putchar(' ');
    printf("\e[u");
    last_line_len = 0;

    // If enabled, print current directory.

    if (cfg_show_dir) {
	printf(COLOR_D_NAME "%s" COLOR_NORMAL ": ", d_current_name);
	last_line_len = d_current_len + 2;
    }
    
    // Now we can print the names of each entry.

    //while ((d_child = readdir(d_current)) != NULL) {
    for (int i = 0; i < d_length; ++i) {
	d_child = d_children[i];

        if (i == selected) {
	    check_entry_selectable(d_child);
            last_line_len += printf("%s%s" COLOR_NORMAL " ",
				    selected_valid ? COLOR_SELECT : COLOR_SELECT_INVALID,
				    d_child->d_name);
	    memcpy(selected_name, d_child->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);
        } else {
            last_line_len += printf("%s%s" COLOR_NORMAL " ",
				    d_child->d_type == DT_DIR ? COLOR_NORMAL : COLOR_NORMAL_INVALID,
				    d_child->d_name);
        }

	free(d_child);
    }

    free(d_children);
    closedir(d_current);
}

int open_selection() {
    // TEMP: Hardcoded to open selection with /usr/bin/xdg-open.

    char * opener = "/usr/bin/xdg-open";
    char * selected_path;
    pid_t pid;

    pid = fork();

    if (pid > 0) {
	return 0;
    } else if (pid == 0) {
	selected_path = append_to_cd(NULL, selected_name);
	execl(opener, opener, selected_path, NULL);
	CHECKBAD(1, 1, "%s failed to execute", opener);
    } else {
	CHECKBAD(1, 1, "Could not start process for %s", opener);
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
    printf("\e[?25l"); // Hide cursor.

    display();
redo:
    switch (getchar()) {
    default: goto redo;
    case 'O':
    case 'o':
	return open_selection();
    case 'Q':
    case 'q':
    case 27: // ESC
        if (getchar() != '[') return 0;
        switch (getchar()) {
        case 'A': // Up
            cd("..");
            break;
        case 'B': // Down
            if (selected_valid) cd(selected_name);
            break;
        case 'D': // Left
            if (--selected < SELECTED_MIN) selected = SELECTED_MAX;
            break;
        case 'C': // Right
            if (++selected > SELECTED_MAX) selected = SELECTED_MIN;
            break;
        }
        break;
    }

    display();
    goto redo;
}
