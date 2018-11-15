#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <termios.h>
#include <unistd.h>

#define COLOR_D_NAME "\e[1m\e[7m"
#define COLOR_NORMAL "\e[m"
#define COLOR_SELECT "\e[7m"

static char * d_current_name = NULL;
static int d_current_len = 0;
static int d_length = 0;

#define SELECTED_MIN 1
#define SELECTED_MAX (d_length)
static int selected = SELECTED_MIN;
#define SELECTED_MAXLEN 256
static char selected_name[SELECTED_MAXLEN];

static struct termios tcattr_old;
static struct termios tcattr_raw;

static void restore_tcattr_old() {
    putchar('\n');
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_old);
}

static void cd(char * to) {
    if (d_current_name == NULL) {
        d_current_name = malloc(sizeof(*d_current_name) * PATH_MAX);
        realpath(to, d_current_name);
    } else if (to[0] == '/') {
        realpath(to, d_current_name);
    } else {
        size_t to_len = strlen(to);
        char * new = malloc(sizeof(*d_current_name) * (d_current_len + to_len + 2));

        if (new == NULL) {
            puts("Out of memory!");
            exit(1); //TEMP
        }

        memcpy(new, d_current_name, sizeof(*d_current_name) * d_current_len);
        new[d_current_len] = '/';
        memcpy(new + d_current_len + 1, to, sizeof(*d_current_name) * to_len);
        new[d_current_len + 1 + to_len] = 0;
        realpath(new, d_current_name);
        free(new);
    }

    d_current_len = strlen(d_current_name);

    selected = SELECTED_MIN;
}

static void display() {
    DIR * d_current;
    struct dirent * d_child;
    int i = -1;

    d_current = opendir(d_current_name);
    if (d_current == NULL) {
        printf("Could not open %s\n", d_current_name);
        exit(1); // TEMP
    }

    printf("\r" COLOR_D_NAME "%s" COLOR_NORMAL ":", d_current_name);
    while ((d_child = readdir(d_current)) != NULL) {
        if (++i == 0) continue;

        if (i == selected) {
            printf(" " COLOR_SELECT "%s" COLOR_NORMAL, d_child->d_name);
            memcpy(selected_name, d_child->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);
        } else {
            printf(" %s", d_child->d_name);
        }
    }

    closedir(d_current);

    d_length = i;
}

int main(int argc, char ** argv) {
    int key = 0;

    if (argc == 1) {
        // Open directory we are run from.
        cd(".");
    } else {
        // TODO: Read flags.
        // TEMP: Assume first arg is dir to peek.
        cd(argv[1]);
    }
    
    // Create raw terminal mode to stop stdin buffer from breaking key press detection.
    // http://pubs.opengroup.org/onlinepubs/000095399/basedefs/termios.h.html#tag_13_74_03_06
    tcgetattr(STDIN_FILENO, &tcattr_old);
    atexit(restore_tcattr_old); // Restore old mode when we're done.
    memcpy(&tcattr_raw, &tcattr_old, sizeof(struct termios));
    tcattr_raw.c_cc[VMIN]  = 0;
    tcattr_raw.c_cc[VTIME] = 0;
    tcattr_raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_raw);

redo:
    switch (getchar()) {
    case 27: // ESC
        if (getchar() != '[') return 0;
        switch (getchar()) {
        case 'A': // Up
            // TODO: if (cangoup)
            cd("..");
            break;
        case 'B': // Down
            // TODO: if (isadir)
            cd(selected_name);
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
