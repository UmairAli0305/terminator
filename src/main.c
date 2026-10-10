
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <limits.h>
#include <sys/wait.h>
#include <signal.h>

#define WIDTH 800
#define HEIGHT 500
#define TERMINATOR_MAX_INPUT 256
#define MAX_LINES 1000
#define LINE_LENGTH 512
#define LINE_HEIGHT 20
#define MAX_TABS 8
#define TAB_WIDTH 150
#define TAB_BAR_HEIGHT 60

typedef struct {
    char lines[MAX_LINES][LINE_LENGTH];
    int count;
    int scroll;
} TextBuffer;

#define HISTORY_MAX 500

typedef struct {
    int used;
    char cwd[PATH_MAX];

    TextBuffer output;
    char input[TERMINATOR_MAX_INPUT];
    int input_len;

    /* Command history for this tab */
    char history[HISTORY_MAX][TERMINATOR_MAX_INPUT];
    int history_count;
    int history_position;
    int last_status;

    pid_t child_pid;
    int output_fd;

    char output_line[LINE_LENGTH];
    size_t output_used;
} Tab;

static Tab tabs[MAX_TABS];
static int tab_count = 0;
static int active_tab = 0;
/* Scrollbar drag state */
static int scrollbar_dragging = 0;
static int scrollbar_drag_offset = 0;

static void add_line(TextBuffer *tb, const char *text)
{
    if (tb->count >= MAX_LINES) {
        memmove(tb->lines, tb->lines + 1,
                (MAX_LINES - 1) * sizeof(tb->lines[0]));
        tb->count--;

        if (tb->scroll > 0)
            tb->scroll--;
    }

    snprintf(tb->lines[tb->count], LINE_LENGTH, "%s", text);
    tb->count++;
}

static void append_output(TextBuffer *tb, char *line,
                          size_t *used, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\r')
            continue;

        if (c == '\n') {
            line[*used] = '\0';
            add_line(tb, line);
            *used = 0;
            line[0] = '\0';
        } else if (*used < LINE_LENGTH - 1) {
            line[(*used)++] = c;
            line[*used] = '\0';
        }
    }
}

static int create_tab(const char *initial_cwd)
{
    if (tab_count >= MAX_TABS)
        return -1;

    int index = tab_count++;
    Tab *tab = &tabs[index];

    memset(tab, 0, sizeof(*tab));
    tab->used = 1;
    tab->output_fd = -1;
    tab->child_pid = -1;

    if (initial_cwd != NULL) {
        snprintf(tab->cwd, sizeof(tab->cwd), "%s", initial_cwd);
    } else if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
        snprintf(tab->cwd, sizeof(tab->cwd), "%s", "/");
    }

    char message[LINE_LENGTH];
    snprintf(message, sizeof(message), "Welcome to Terminator - Tab %d", index + 1);
    add_line(&tab->output, message);
    add_line(&tab->output, "Try: pwd, ls, date, sleep 5");
    add_line(&tab->output, "");

    return index;
}

static void redraw(Display *display, Window window, GC gc)
{
    Tab *tab = &tabs[active_tab];

    XClearWindow(display, window);

    XDrawString(display, window, gc, 15, 20,
                "Terminator - Custom Terminal", 29);

    
/* Draw the tabs and their close controls. */
for (int i = 0; i < tab_count; i++) {
    char label[LINE_LENGTH];

    const char *base = strrchr(tabs[i].cwd, '/');
    base = (base != NULL && base[1] != '\0') ? base + 1 : "/";

    snprintf(label, sizeof(label), "[ %d: %.10s",
             i + 1, base);

    int tab_x = 15 + i * TAB_WIDTH;

    XDrawString(display, window, gc,
                tab_x, 48, label, (int)strlen(label));

    /* Close control for this tab. */
    XDrawString(display, window, gc,
                tab_x + 120, 48, "x", 1);

    if (i == active_tab) {
        XDrawLine(display, window, gc,
                  tab_x, 54, tab_x + 140, 54);
    }
}

    int plus_x = 15 + tab_count * TAB_WIDTH;
    XDrawString(display, window, gc, plus_x, 48, "[ + ]", 5);

    XDrawLine(display, window, gc, 10, TAB_BAR_HEIGHT,
              WIDTH - 10, TAB_BAR_HEIGHT);

    int visible = (HEIGHT - TAB_BAR_HEIGHT - 45) / LINE_HEIGHT;
    int max_scroll = tab->output.count > visible
                     ? tab->output.count - visible : 0;

    if (tab->output.scroll > max_scroll)
        tab->output.scroll = max_scroll;

    int start = max_scroll - tab->output.scroll;
    int end = start + visible;

    if (end > tab->output.count)
        end = tab->output.count;
    /* Draw scrollbar on the right side. */
        int track_x = WIDTH - 18;
        int track_y = TAB_BAR_HEIGHT + 10;
        int track_height = HEIGHT - TAB_BAR_HEIGHT - 55;

        XDrawRectangle(display, window, gc,
                    track_x, track_y, 8, track_height);

        if (tab->output.count > 0 && visible > 0) {
            int thumb_height =
                (visible * track_height) / tab->output.count;

            if (thumb_height < 20)
                thumb_height = 20;

            if (thumb_height > track_height)
                thumb_height = track_height;

            int thumb_range = track_height - thumb_height;

            int thumb_y = track_y;

            if (max_scroll > 0) {
                thumb_y += (tab->output.scroll * thumb_range)
                        / max_scroll;
            }

            XFillRectangle(display, window, gc,
                        track_x + 1, thumb_y + 1,
                        7, thumb_height - 1);
        }

    int y = TAB_BAR_HEIGHT + 22;

    for (int i = start; i < end; i++) {
        XDrawString(display, window, gc, 15, y,
                    tab->output.lines[i],
                    (int)strlen(tab->output.lines[i]));
        y += LINE_HEIGHT;
    }

    XDrawLine(display, window, gc, 10, HEIGHT - 35,
              WIDTH - 10, HEIGHT - 35);

    char prompt[LINE_LENGTH];
    snprintf(prompt, sizeof(prompt), "umair@ubuntu:%.35s> ", tab->cwd);

    XDrawString(display, window, gc, 15, HEIGHT - 15,
                prompt, (int)strlen(prompt));

    int prompt_width = XTextWidth(
        XQueryFont(display, XGContextFromGC(gc)),
        prompt, (int)strlen(prompt)
    );

    XDrawString(display, window, gc, 15 + prompt_width,
                HEIGHT - 15, tab->input, tab->input_len);

    XFlush(display);
}

static int change_directory(Tab *tab, const char *dir)
{
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];

    if (dir == NULL || dir[0] == '\0') {
        dir = getenv("HOME");
        if (dir == NULL)
            dir = "/";
    }

    int written;

    if (dir[0] == '/') {
        written = snprintf(candidate, sizeof(candidate), "%s", dir);
    } else {
        written = snprintf(candidate, sizeof(candidate),
                           "%s/%s", tab->cwd, dir);
    }

    if (written < 0 || (size_t)written >= sizeof(candidate)) {
        add_line(&tab->output, "cd: path is too long");
        return 1;
    }

    if (realpath(candidate, resolved) == NULL) {
        add_line(&tab->output, "cd: directory not found");
        return 1;
    }

    snprintf(tab->cwd, sizeof(tab->cwd), "%s", resolved);
    return 0;
}
static void save_history(Tab *tab, const char *command)
{
    if (command == NULL || command[0] == '\0')
        return;

    /* If history is full, remove the oldest command. */
    if (tab->history_count >= HISTORY_MAX) {
        memmove(tab->history,
                tab->history + 1,
                (HISTORY_MAX - 1) * sizeof(tab->history[0]));

        tab->history_count = HISTORY_MAX - 1;
    }

    snprintf(tab->history[tab->history_count],
             TERMINATOR_MAX_INPUT,
             "%s", command);

    tab->history_count++;

    /* The next Up key should recall the latest command. */
    tab->history_position = tab->history_count;
}
static void expand_last_status(const char *input,
                               int status,
                               char *output,
                               size_t output_size)
{
    char status_text[16];
    snprintf(status_text, sizeof(status_text), "%d", status);

    size_t i = 0;
    size_t j = 0;
    size_t status_len = strlen(status_text);

    while (input[i] != '\0' && j + 1 < output_size) {
        if (input[i] == '$' && input[i + 1] == '?') {
            for (size_t k = 0;
                 k < status_len && j + 1 < output_size; k++) {
                output[j++] = status_text[k];
            }
            i += 2;
        } else {
            output[j++] = input[i++];
        }
    }

    output[j] = '\0';
}


static pid_t start_command(Tab *tab, char *command)
{
    char *args[64];
    int argc = 0;

    char *token = strtok(command, " \t");

    while (token != NULL && argc < 63) {
        args[argc++] = token;
        token = strtok(NULL, " \t");
    }

    args[argc] = NULL;

    if (argc == 0)
        return -1;

    if (strcmp(args[0], "cd") == 0) {
    tab->last_status =
        change_directory(tab, argc > 1 ? args[1] : NULL);

    return -1;
    }
    if (strcmp(args[0], "history") == 0) {
    char line[LINE_LENGTH];

    for (int i = 0; i < tab->history_count; i++) {
        snprintf(line, sizeof(line), "%4d  %s",
                 i + 1, tab->history[i]);

        add_line(&tab->output, line);
    }
    tab->last_status = 0;
    return -1;
    }

    if (strcmp(args[0], "exit") == 0) { // now when you type "exit" --> it will close the current tab -> we tell -2 to close the current tab 
        //Line number 793
    return -2;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        add_line(&tab->output, "Error: pipe failed");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        add_line(&tab->output, "Error: fork failed");
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (chdir(tab->cwd) == -1)
            _exit(126);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1 ||
            dup2(pipefd[1], STDERR_FILENO) == -1)
            _exit(126);

        close(pipefd[1]);

        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            close(nullfd);
        }

        execvp(args[0], args);
        perror(args[0]);
        _exit(127);
    }

    close(pipefd[1]);

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    tab->output_fd = pipefd[0];
    add_line(&tab->output, "[Command started]");

    return pid;
}

static void finish_command(Tab *tab)
{
    if (tab->output_used > 0) {
        tab->output_line[tab->output_used] = '\0';
        add_line(&tab->output, tab->output_line);
        tab->output_used = 0;
        tab->output_line[0] = '\0';
    }

    if (tab->output_fd >= 0) {
        close(tab->output_fd);
        tab->output_fd = -1;
    }

    if (tab->child_pid > 0) {
    int status = 0;
    pid_t result;

    do {
        result = waitpid(tab->child_pid, &status, 0);
    } while (result == -1 && errno == EINTR);
    if (result > 0) {
        if (WIFEXITED(status)) {
            tab->last_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            tab->last_status = 128 + WTERMSIG(status);
        }
    }
    tab->child_pid = -1;
    }

    add_line(&tab->output, "[Command finished]");
    tab->output.scroll = 0;
}

/* Close one tab and release its resources. */
static int close_tab(int index)
{
    if (index < 0 || index >= tab_count)
        return 0;

    Tab *tab = &tabs[index];

    /* Close the pipe and stop this tab's running command. */
    if (tab->output_fd >= 0) {
        close(tab->output_fd);
        tab->output_fd = -1;
    }

    if (tab->child_pid > 0) {
        kill(tab->child_pid, SIGTERM);
        waitpid(tab->child_pid, NULL, 0);
        tab->child_pid = -1;
    }

    /* Remove the tab by shifting later tabs left. */
    for (int i = index; i < tab_count - 1; i++)
        tabs[i] = tabs[i + 1];

    tab_count--;

    memset(&tabs[tab_count], 0, sizeof(tabs[tab_count]));
    tabs[tab_count].output_fd = -1;
    tabs[tab_count].child_pid = -1;

    if (tab_count == 0)
        return 1;  /* No tabs remain: close the application. */

    if (active_tab > index)
        active_tab--;
    else if (active_tab >= tab_count)
        active_tab = tab_count - 1;

    return 0;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "Cannot open X display. Use SSH with X11 forwarding.\n");
        return 1;
    }

    int screen = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        100, 100, WIDTH, HEIGHT, 1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XStoreName(display, window, "Terminator - Custom Terminal");
    XSelectInput(display, window,
             ExposureMask | KeyPressMask | ButtonPressMask |
             ButtonReleaseMask | PointerMotionMask |
             StructureNotifyMask);
    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, NULL);

    create_tab(NULL);
    active_tab = 0;

    int running = 1;
    int x_fd = ConnectionNumber(display);

    while (running) {
        struct pollfd fds[1 + MAX_TABS];
        int tab_for_fd[1 + MAX_TABS];
        int nfds = 1;

        fds[0].fd = x_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        tab_for_fd[0] = -1;

        for (int i = 0; i < tab_count; i++) {
            if (tabs[i].output_fd >= 0) {
                fds[nfds].fd = tabs[i].output_fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                tab_for_fd[nfds] = i;
                nfds++;
            }
        }

        int result;

        /* Don't sleep if Xlib already has queued events. */
        if (XPending(display) > 0)
            result = 0;
        else
            result = poll(fds, nfds, -1);

        if (result < 0) {
            if (errno == EINTR)
                continue;

            perror("poll");
            break;
        }

        for (int j = 1; j < nfds; j++) {
            if (!(fds[j].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            int index = tab_for_fd[j];
            Tab *tab = &tabs[index];
            char chunk[1024];
            ssize_t n;

            while ((n = read(tab->output_fd, chunk, sizeof(chunk))) > 0) {
                append_output(&tab->output, tab->output_line,
                              &tab->output_used, chunk, (size_t)n);
                tab->output.scroll = 0;
            }

            if (n == 0 ||
                (n < 0 && errno != EAGAIN &&
                 errno != EWOULDBLOCK && errno != EINTR)) {
                finish_command(tab);
            }

            if (index == active_tab)
                redraw(display, window, gc);
        }

        while (XPending(display) > 0) {
    XEvent event;
    XNextEvent(display, &event);

    if (event.type == Expose) {
        redraw(display, window, gc);

    } else if (event.type == ButtonPress) {
        int x = event.xbutton.x;
        int y = event.xbutton.y;

        int track_x = WIDTH - 18;
        int track_y = TAB_BAR_HEIGHT + 10;
        int track_height = HEIGHT - TAB_BAR_HEIGHT - 55;

        /* Handle scrollbar clicks. */
        if (x >= track_x - 4 && x <= track_x + 12 &&
            y >= track_y && y <= track_y + track_height) {

            Tab *tab = &tabs[active_tab];

            int visible =
                (HEIGHT - TAB_BAR_HEIGHT - 45) / LINE_HEIGHT;

            int max_scroll = tab->output.count > visible
                ? tab->output.count - visible : 0;

            int thumb_height = tab->output.count > 0
                ? visible * track_height / tab->output.count
                : track_height;

            if (thumb_height < 20)
                thumb_height = 20;
            if (thumb_height > track_height)
                thumb_height = track_height;

            int thumb_range = track_height - thumb_height;
            int thumb_y = track_y;

            if (max_scroll > 0)
                thumb_y += tab->output.scroll * thumb_range / max_scroll;

            if (y < thumb_y) {
                tab->output.scroll = max_scroll;
                scrollbar_drag_offset = thumb_height / 2;
            } else if (y >= thumb_y + thumb_height) {
                tab->output.scroll = 0;
                scrollbar_drag_offset = thumb_height / 2;
            } else {
                scrollbar_drag_offset = y - thumb_y;
            }

            scrollbar_dragging = 1;
            redraw(display, window, gc);

        } else if (y >= 30 && y <= 55) {
            int handled = 0;

            /* Close a tab. */
            for (int i = 0; i < tab_count; i++) {
                int tab_x = 15 + i * TAB_WIDTH;

                if (x >= tab_x + 115 && x <= tab_x + 140) {
                    running = close_tab(i) == 0;
                    handled = 1;
                    break;
                }
            }

            if (!handled) {
                /* Switch tabs. */
                for (int i = 0; i < tab_count; i++) {
                    int tab_x = 15 + i * TAB_WIDTH;

                    if (x >= tab_x && x < tab_x + TAB_WIDTH) {
                        active_tab = i;
                        handled = 1;
                        break;
                    }
                }

                /* Create a tab. */
                if (!handled &&
                    x >= 15 + tab_count * TAB_WIDTH &&
                    tab_count < MAX_TABS) {
                    int new_index = create_tab(tabs[active_tab].cwd);

                    if (new_index >= 0)
                        active_tab = new_index;
                }
            }

            if (running)
                redraw(display, window, gc);
        }

    } else if (event.type == MotionNotify && scrollbar_dragging) {
        Tab *tab = &tabs[active_tab];

        int track_y = TAB_BAR_HEIGHT + 10;
        int track_height = HEIGHT - TAB_BAR_HEIGHT - 55;

        int visible =
            (HEIGHT - TAB_BAR_HEIGHT - 45) / LINE_HEIGHT;

        int max_scroll = tab->output.count > visible
            ? tab->output.count - visible : 0;

        int thumb_height = tab->output.count > 0
            ? visible * track_height / tab->output.count
            : track_height;

        if (thumb_height < 20)
            thumb_height = 20;
        if (thumb_height > track_height)
            thumb_height = track_height;

        int thumb_range = track_height - thumb_height;
        int new_thumb_y = event.xmotion.y - scrollbar_drag_offset;

        if (new_thumb_y < track_y)
            new_thumb_y = track_y;
        if (new_thumb_y > track_y + thumb_range)
            new_thumb_y = track_y + thumb_range;

        tab->output.scroll = thumb_range > 0
            ? (new_thumb_y - track_y) * max_scroll / thumb_range
            : 0;

        redraw(display, window, gc);

    } else if (event.type == ButtonRelease) {
        scrollbar_dragging = 0;

    } else if (event.type == KeyPress) {
        Tab *tab = &tabs[active_tab];

        char buffer[32];
        KeySym keysym;

        int count = XLookupString(
            &event.xkey, buffer, sizeof(buffer), &keysym, NULL
        );

        if (keysym == XK_Escape) {
            running = 0;
            break;

        } else if (keysym == XK_BackSpace) {
            if (tab->input_len > 0)
                tab->input[--tab->input_len] = '\0';

        } else if (keysym == XK_Up) {
            if (tab->history_position > 0) {
                tab->history_position--;

                memmove(tab->input,
                        tab->history[tab->history_position],
                        strlen(tab->history[tab->history_position]) + 1);

                tab->input_len = (int)strlen(tab->input);
            }

        } else if (keysym == XK_Down) {
            if (tab->history_position < tab->history_count) {
                tab->history_position++;

                if (tab->history_position == tab->history_count) {
                    tab->input[0] = '\0';
                    tab->input_len = 0;
                } else {
                    memmove(tab->input,
                            tab->history[tab->history_position],
                            strlen(tab->history[tab->history_position]) + 1);

                    tab->input_len = (int)strlen(tab->input);
                }
            }

        } else if (keysym == XK_Return) {
            char prompt_line[LINE_LENGTH + TERMINATOR_MAX_INPUT];

            snprintf(prompt_line, sizeof(prompt_line),
                     "umair@ubuntu:%.35s> %s",
                     tab->cwd, tab->input);

            add_line(&tab->output, prompt_line);

            if (tab->child_pid > 0) {
                add_line(&tab->output,
                         "A command is already running in this tab.");
            } else {
                char command[TERMINATOR_MAX_INPUT];
                char expanded_command[TERMINATOR_MAX_INPUT];

                snprintf(command, sizeof(command), "%s", tab->input);

                /* Save the original command in history. */
                save_history(tab, command);

                /* Expand $? using the previous exit status. */
                expand_last_status(command, tab->last_status,
                expanded_command, sizeof(expanded_command));
                pid_t command_result = start_command(tab, expanded_command);

                if (command_result == -2) {
                    running = close_tab(active_tab) == 0; // here we explicitly declared  -2 closes the current tab
                    if (running) {
                        redraw(display, window, gc);
                    }
                    if (!running) {
                        break;
                    }
                    continue;
                }
                tab->child_pid = command_result;
                //
                tab->output_used = 0;
                tab->output_line[0] = '\0';
            }

            tab->input_len = 0;
            tab->input[0] = '\0';
            tab->output.scroll = 0;

        } else if (count > 0 &&
                   tab->input_len + count < TERMINATOR_MAX_INPUT) {
            for (int i = 0; i < count; i++)
                tab->input[tab->input_len++] = buffer[i];

            tab->input[tab->input_len] = '\0';
        }

        redraw(display, window, gc);
        }
    }
    }

    for (int i = 0; i < tab_count; i++) {
        if (tabs[i].output_fd >= 0)
            close(tabs[i].output_fd);

        if (tabs[i].child_pid > 0) {
            kill(tabs[i].child_pid, SIGTERM);
            waitpid(tabs[i].child_pid, NULL, 0);
        }
    }

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}
