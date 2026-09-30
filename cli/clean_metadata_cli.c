// clean_metadata_cli: an interactive, terminal-based equivalent of the
// Clean Metadata GUI. Like the GUI, it is a thin approval layer on top of
// the `clean_metadata` scanner: it runs `clean_metadata --scan <folder>`,
// lists the matches, lets the user pick which ones to remove, and deletes
// the approved items itself (in parallel), reporting a status per item.
#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define MAX_PATH 4096
#define CLI_NAME "clean_metadata"
#define MARKER_NAME ".metadata_never_index"

extern char **environ;

// MARK: - Model

// Lifecycle of a single matched item as the user approves and (optionally)
// removes it.
typedef enum {
    STATUS_PENDING,
    STATUS_REMOVED,
    STATUS_FAILED
} ItemStatus;

// One file or folder matched by `clean_metadata --scan`.
typedef struct {
    char *path;
    int is_dir;
    int selected;
    ItemStatus status;
    int error;          // errno of the failure when status == STATUS_FAILED
} MatchItem;

typedef struct {
    char *root;         // absolute path of the chosen folder, or NULL
    MatchItem *items;
    size_t count;
    int scanned;        // a scan of root completed successfully
    char summary[256];  // last summary line, empty if none
} AppState;

static AppState state = { NULL, NULL, 0, 0, "" };

static int use_color = 0;
#define C_RESET  (use_color ? "\033[0m"  : "")
#define C_BOLD   (use_color ? "\033[1m"  : "")
#define C_DIM    (use_color ? "\033[2m"  : "")
#define C_RED    (use_color ? "\033[31m" : "")
#define C_GREEN  (use_color ? "\033[32m" : "")

static void clear_items(void) {
    for (size_t i = 0; i < state.count; i++) {
        free(state.items[i].path);
    }
    free(state.items);
    state.items = NULL;
    state.count = 0;
    state.scanned = 0;
}

static size_t selected_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < state.count; i++) {
        if (state.items[i].selected && state.items[i].status == STATUS_PENDING) n++;
    }
    return n;
}

static const char *display_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// MARK: - Input helpers

// Reads one line from stdin (without the trailing newline). Returns NULL on
// EOF; the caller owns the returned buffer.
static char *read_line(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);

    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, stdin);
    if (len < 0) {
        free(line);
        return NULL;
    }
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    return line;
}

// Asks a yes/no question; only an explicit 'y'/'Y' counts as yes.
static int confirm(const char *prompt) {
    char *answer = read_line(prompt);
    int yes = answer && (answer[0] == 'y' || answer[0] == 'Y');
    if (!answer) printf("\n");
    free(answer);
    return yes;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

// Normalises a path typed (or drag-and-dropped) into the terminal: strips
// surrounding quotes, or else undoes backslash escapes ("My\ Folder"), and
// expands a leading "~". Writes the result into out.
static void normalise_path(const char *input, char *out, size_t out_size) {
    char buf[MAX_PATH];
    size_t len = strlen(input);

    if (len >= 2 && (input[0] == '\'' || input[0] == '"') && input[len - 1] == input[0]) {
        snprintf(buf, sizeof(buf), "%.*s", (int)(len - 2), input + 1);
    } else {
        size_t j = 0;
        for (size_t i = 0; input[i] && j + 1 < sizeof(buf); i++) {
            if (input[i] == '\\' && input[i + 1]) i++;
            buf[j++] = input[i];
        }
        buf[j] = '\0';
    }

    const char *home = getenv("HOME");
    if (home && buf[0] == '~' && (buf[1] == '/' || buf[1] == '\0')) {
        snprintf(out, out_size, "%s%s", home, buf + 1);
    } else {
        snprintf(out, out_size, "%s", buf);
    }
}

// MARK: - Locating and running the scanner

// Resolved path of this executable, or "" if it can't be determined.
static void executable_path(const char *argv0, char *out, size_t out_size) {
    char raw[MAX_PATH] = "";
#if defined(__APPLE__)
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) raw[0] = '\0';
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", raw, sizeof(raw) - 1);
    raw[n > 0 ? n : 0] = '\0';
#endif
    if (raw[0] == '\0' && strchr(argv0, '/')) {
        snprintf(raw, sizeof(raw), "%s", argv0);
    }

    char resolved[PATH_MAX];
    if (raw[0] == '\0' || realpath(raw, resolved) == NULL) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%s", resolved);
}

// This program is itself installed as `clean_metadata` (in ~/bin), so a
// candidate that is this very executable must be skipped rather than run
// as the scanner.
static struct stat self_stat;
static int have_self_stat = 0;

static int try_candidate(const char *dir, const char *suffix, char *out, size_t out_size) {
    if (!dir || dir[0] == '\0') return 0;
    snprintf(out, out_size, "%s%s/%s", dir, suffix, CLI_NAME);
    struct stat st;
    if (stat(out, &st) != 0 || !S_ISREG(st.st_mode) || access(out, X_OK) != 0) return 0;
    return !(have_self_stat && st.st_dev == self_stat.st_dev && st.st_ino == self_stat.st_ino);
}

// Looks for the `clean_metadata` scanner: next to this executable first,
// then ../libexec/clean_metadata relative to it (where `make install` puts
// the scanner), the current directory and its parent (useful when run from
// the repo), ~/libexec/clean_metadata, /usr/local/bin, the copy bundled in
// /Applications/Clean Metadata.app, and finally $PATH.
static int locate_cli_binary(const char *argv0, char *out, size_t out_size) {
    char self_dir[MAX_PATH];
    executable_path(argv0, self_dir, sizeof(self_dir));
    have_self_stat = self_dir[0] != '\0' && stat(self_dir, &self_stat) == 0;
    char *slash = strrchr(self_dir, '/');
    if (slash) *slash = '\0';

    if (try_candidate(self_dir, "", out, out_size)) return 1;
    if (try_candidate(self_dir, "/../libexec/clean_metadata", out, out_size)) return 1;

    char cwd[MAX_PATH];
    if (getcwd(cwd, sizeof(cwd))) {
        if (try_candidate(cwd, "", out, out_size)) return 1;
        if (try_candidate(cwd, "/..", out, out_size)) return 1;
    }

    if (try_candidate(getenv("HOME"), "/libexec/clean_metadata", out, out_size)) return 1;
    if (try_candidate("/usr/local/bin", "", out, out_size)) return 1;
#if defined(__APPLE__)
    // The copy bundled inside the GUI app (see `make install-gui`).
    if (try_candidate("/Applications/Clean Metadata.app/Contents/Resources", "", out, out_size)) return 1;
#endif

    const char *path_env = getenv("PATH");
    if (path_env) {
        char *copy = strdup(path_env);
        char *saveptr = NULL;
        for (char *dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
            if (try_candidate(dir, "", out, out_size)) {
                free(copy);
                return 1;
            }
        }
        free(copy);
    }
    return 0;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static int buffer_append(Buffer *b, const char *src, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *data = realloc(b->data, cap);
        if (!data) return -1;
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int compare_items(const void *a, const void *b) {
    return strcmp(((const MatchItem *)a)->path, ((const MatchItem *)b)->path);
}

// Runs `clean_metadata --scan <root>` and parses its NUL-delimited stdout
// into state.items (sorted). Stdout and stderr are drained together with
// poll() so a large scan can't deadlock on a full pipe buffer. On failure,
// writes a message into err and returns -1.
static int run_scan(const char *binary, const char *root, char *err, size_t err_size) {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) {
        snprintf(err, err_size, "pipe: %s", strerror(errno));
        return -1;
    }
    if (pipe(err_pipe) != 0) {
        snprintf(err, err_size, "pipe: %s", strerror(errno));
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

    char *args[] = { (char *)binary, "--scan", (char *)root, NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, binary, &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (rc != 0) {
        snprintf(err, err_size, "Failed to launch %s: %s", binary, strerror(rc));
        close(out_pipe[0]);
        close(err_pipe[0]);
        return -1;
    }

    Buffer out = { NULL, 0, 0 };
    Buffer errbuf = { NULL, 0, 0 };
    struct pollfd fds[2] = {
        { out_pipe[0], POLLIN, 0 },
        { err_pipe[0], POLLIN, 0 },
    };
    Buffer *targets[2] = { &out, &errbuf };
    int open_fds = 2;
    char chunk[1 << 16];

    while (open_fds > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(fds[i].fd, chunk, sizeof(chunk));
            if (n > 0) {
                buffer_append(targets[i], chunk, (size_t)n);
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                open_fds--;
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        if (fds[i].fd >= 0) close(fds[i].fd);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char *msg = errbuf.data ? trim(errbuf.data) : NULL;
        if (msg && *msg) {
            snprintf(err, err_size, "%s", msg);
        } else if (WIFEXITED(status)) {
            snprintf(err, err_size, "%s exited with status %d", CLI_NAME, WEXITSTATUS(status));
        } else {
            snprintf(err, err_size, "%s terminated abnormally", CLI_NAME);
        }
        free(out.data);
        free(errbuf.data);
        return -1;
    }

    size_t count = 0;
    for (size_t i = 0; i < out.len; i++) {
        if (out.data[i] == '\0' && (i == 0 || out.data[i - 1] != '\0')) count++;
    }

    MatchItem *items = count ? calloc(count, sizeof(MatchItem)) : NULL;
    size_t n = 0;
    for (size_t pos = 0; pos < out.len && n < count;) {
        size_t len = strlen(out.data + pos);
        if (len > 0) {
            MatchItem *item = &items[n++];
            item->path = strdup(out.data + pos);
            item->selected = 1;
            item->status = STATUS_PENDING;
            struct stat st;
            item->is_dir = stat(item->path, &st) == 0 && S_ISDIR(st.st_mode);
        }
        pos += len + 1;
    }
    if (n > 1) qsort(items, n, sizeof(MatchItem), compare_items);

    state.items = items;
    state.count = n;
    free(out.data);
    free(errbuf.data);
    return 0;
}

// MARK: - Deletion

// Recursively removes path. Keeps going past individual failures inside a
// directory so as much as possible is removed; returns 0 on success or the
// first errno encountered.
static int remove_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno;

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : errno;
    }

    int first_error = 0;
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char sub[MAX_PATH];
            snprintf(sub, sizeof(sub), "%s/%s", path, entry->d_name);
            int e = remove_recursive(sub);
            if (e && !first_error) first_error = e;
        }
        closedir(dir);
    } else {
        first_error = errno;
    }

    if (rmdir(path) != 0) {
        return first_error ? first_error : errno;
    }
    return 0;
}

// Selected items are handed out to worker threads via a shared index; each
// worker records its outcome directly on the item it removed.
typedef struct {
    MatchItem **items;
    size_t count;
    size_t next;
    pthread_mutex_t lock;
} DeleteJob;

static void *delete_worker(void *arg) {
    DeleteJob *job = arg;
    for (;;) {
        pthread_mutex_lock(&job->lock);
        if (job->next >= job->count) {
            pthread_mutex_unlock(&job->lock);
            return NULL;
        }
        MatchItem *item = job->items[job->next++];
        pthread_mutex_unlock(&job->lock);

        int e = remove_recursive(item->path);
        item->status = e ? STATUS_FAILED : STATUS_REMOVED;
        item->error = e;
    }
}

static int get_num_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    if (n > 32) n = 32;
    return (int)n;
}

// MARK: - Index inhibition marker

static void marker_path(char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", state.root, MARKER_NAME);
}

static int marker_exists(void) {
    char path[MAX_PATH];
    marker_path(path, sizeof(path));
    struct stat st;
    return lstat(path, &st) == 0;
}

// Mirrors the scanner's index-inhibition marker: create an empty
// .metadata_never_index at the scanned root, never overwriting an existing one.
static void offer_marker(void) {
    if (!state.root || !state.scanned || marker_exists()) return;

    printf("\n%s has no %s marker.\n", state.root, MARKER_NAME);
    printf("Creating this empty file stops Spotlight from indexing this location.\n");
    if (!confirm("Create .metadata_never_index? [y/N]: ")) {
        return;
    }

    char path[MAX_PATH];
    marker_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0 || errno == EEXIST) {
        if (fd >= 0) close(fd);
        size_t len = strlen(state.summary);
        snprintf(state.summary + len, sizeof(state.summary) - len,
                 "%sCreated %s marker.", len ? " " : "", MARKER_NAME);
        printf("%s\n", state.summary);
    } else {
        printf("%sCould not create %s: %s%s\n", C_RED, MARKER_NAME, strerror(errno), C_RESET);
    }
}

// MARK: - Views

static void print_header(void) {
    printf("\n%sFolder:%s %s\n", C_BOLD, C_RESET, state.root ? state.root : "No folder selected");
}

static void print_items(void) {
    if (state.count == 0) {
        if (state.summary[0]) {
            printf("%s\n", state.summary);
        } else if (!state.scanned) {
            printf("Choose a folder (f) and scan (s) to look for hidden metadata files\n"
                   "(.DS_Store, .Spotlight-V100, Thumbs.db, and similar clutter).\n");
        }
        return;
    }

    printf("\n%s%zu item(s) found — %zu selected%s\n\n", C_BOLD, state.count, selected_count(), C_RESET);

    int width = snprintf(NULL, 0, "%zu", state.count);
    for (size_t i = 0; i < state.count; i++) {
        const MatchItem *item = &state.items[i];
        const char *box = item->status != STATUS_PENDING ? "[-]" : item->selected ? "[x]" : "[ ]";
        const char *dim = item->status == STATUS_REMOVED ? C_DIM : "";

        printf("%s%*zu %s %-6s %s%s", dim, width, i + 1, box,
               item->is_dir ? "folder" : "file", display_name(item->path), C_RESET);

        switch (item->status) {
        case STATUS_PENDING:
            break;
        case STATUS_REMOVED:
            printf("  %s✓ removed%s", C_GREEN, C_RESET);
            break;
        case STATUS_FAILED:
            printf("  %s✗ %s%s", C_RED, strerror(item->error), C_RESET);
            break;
        }
        printf("\n%s%*s %s%s\n", C_DIM, width + 11, "", item->path, C_RESET);
    }

    if (state.summary[0]) {
        printf("\n%s\n", state.summary);
    }
}

static void print_help(void) {
    printf("\nCommands:\n"
           "  f [path]      Choose a folder to scan (prompts if no path given)\n"
           "  s             Scan the chosen folder\n"
           "  l             List the scan results\n"
           "  t N [N-M ...] Toggle selection of items by number or range\n"
           "  a             Select all\n"
           "  n             Select none\n"
           "  r             Remove selected items (asks for confirmation)\n"
           "  h             Show this help\n"
           "  q             Exit\n");
}

// MARK: - Actions

static void choose_folder(const char *arg) {
    char *typed = NULL;
    if (!arg || !*arg) {
        typed = read_line("Folder to scan: ");
        if (!typed) {
            printf("\n");
            return;
        }
        arg = trim(typed);
        if (!*arg) {
            free(typed);
            return;
        }
    }

    char path[MAX_PATH];
    normalise_path(arg, path, sizeof(path));
    free(typed);

    char resolved[PATH_MAX];
    struct stat st;
    if (realpath(path, resolved) == NULL || stat(resolved, &st) != 0) {
        printf("%sError: '%s': %s%s\n", C_RED, path, strerror(errno), C_RESET);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        printf("%sError: '%s' is not a directory.%s\n", C_RED, resolved, C_RESET);
        return;
    }

    free(state.root);
    state.root = strdup(resolved);
    clear_items();
    state.summary[0] = '\0';
    print_header();
}

static void scan(const char *argv0) {
    if (!state.root) {
        printf("Choose a folder first (f).\n");
        return;
    }

    char binary[MAX_PATH];
    if (!locate_cli_binary(argv0, binary, sizeof(binary))) {
        printf("%sCould not find the %s command-line tool. Build it (make) in the repo\n"
               "root first — see the README — or put it next to this program.%s\n",
               C_RED, CLI_NAME, C_RESET);
        return;
    }

    clear_items();
    state.summary[0] = '\0';
    printf("Scanning %s ...\n", state.root);
    fflush(stdout);

    char err[1024];
    if (run_scan(binary, state.root, err, sizeof(err)) != 0) {
        printf("%s%s%s\n", C_RED, err, C_RESET);
        return;
    }

    state.scanned = 1;
    if (state.count == 0) {
        snprintf(state.summary, sizeof(state.summary),
                 "No target housekeeping files or directories found.");
    }
    print_items();
    offer_marker();
}

// Applies "t" arguments: space/comma-separated numbers and N-M ranges.
static void toggle(char *args) {
    if (state.count == 0) {
        printf("Nothing to toggle; scan a folder first.\n");
        return;
    }
    if (!args || !*args) {
        printf("Usage: t N [N-M ...]\n");
        return;
    }

    char *saveptr = NULL;
    for (char *tok = strtok_r(args, " ,\t", &saveptr); tok; tok = strtok_r(NULL, " ,\t", &saveptr)) {
        char *end;
        long lo = strtol(tok, &end, 10);
        long hi = lo;
        if (*end == '-') hi = strtol(end + 1, &end, 10);
        if (*end != '\0' || lo < 1 || hi < lo || (size_t)hi > state.count) {
            printf("Ignoring '%s' (expected a number or range within 1-%zu).\n", tok, state.count);
            continue;
        }
        for (long i = lo; i <= hi; i++) {
            MatchItem *item = &state.items[i - 1];
            if (item->status == STATUS_PENDING) item->selected = !item->selected;
        }
    }
    printf("%zu item(s) found — %zu selected\n", state.count, selected_count());
}

static void select_all(int selected) {
    for (size_t i = 0; i < state.count; i++) {
        if (state.items[i].status == STATUS_PENDING) state.items[i].selected = selected;
    }
    printf("%zu item(s) found — %zu selected\n", state.count, selected_count());
}

static void remove_selected(void) {
    size_t n = selected_count();
    if (n == 0) {
        printf("Nothing selected.\n");
        return;
    }

    printf("\nDelete %zu item(s)?\n"
           "This permanently deletes the selected files and folders (recursively, where\n"
           "applicable). This cannot be undone.\n", n);
    if (!confirm("Delete? [y/N]: ")) {
        printf("Cancelled.\n");
        return;
    }

    MatchItem **work = malloc(n * sizeof(MatchItem *));
    size_t k = 0;
    for (size_t i = 0; i < state.count; i++) {
        MatchItem *item = &state.items[i];
        if (item->selected && item->status == STATUS_PENDING) work[k++] = item;
    }

    DeleteJob job = { work, n, 0, PTHREAD_MUTEX_INITIALIZER };

    // Ignore Ctrl-C while removing so quitting can't abandon a
    // half-finished batch of deletions.
    struct sigaction ignore = { 0 }, old_int;
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore, &old_int);

    printf("Removing %zu item(s) ...\n", n);
    fflush(stdout);

    int threads = get_num_threads();
    if ((size_t)threads > n) threads = (int)n;
    pthread_t tids[threads];
    for (int i = 0; i < threads; i++) {
        pthread_create(&tids[i], NULL, delete_worker, &job);
    }
    for (int i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }

    sigaction(SIGINT, &old_int, NULL);

    size_t succeeded = 0, failed = 0;
    for (size_t i = 0; i < n; i++) {
        if (work[i]->status == STATUS_REMOVED) succeeded++;
        else failed++;
    }
    free(work);

    if (failed == 0) {
        snprintf(state.summary, sizeof(state.summary), "Removed %zu item(s).", succeeded);
    } else {
        snprintf(state.summary, sizeof(state.summary),
                 "Removed %zu item(s), %zu failed. See the status column for details.",
                 succeeded, failed);
    }
    print_items();
}

// MARK: - Main loop

int main(int argc, char *argv[]) {
    if (argc > 2 || (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))) {
        fprintf(stderr, "Usage: %s [folder]\n", argv[0]);
        fprintf(stderr, "Interactively scan a folder for hidden metadata files and remove the ones you approve.\n");
        fprintf(stderr, "If a folder is given, it is scanned immediately.\n");
        return argc == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    use_color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;

    printf("%sClean Metadata%s — interactive cleanup of hidden metadata files\n", C_BOLD, C_RESET);

    if (argc == 2) {
        choose_folder(argv[1]);
        if (state.root) scan(argv[0]);
    } else {
        print_header();
        print_items();
    }
    print_help();

    for (;;) {
        char *line = read_line("\n> ");
        if (!line) {
            printf("\n");
            break;
        }

        char *cmd = trim(line);
        char *args = cmd;
        while (*args && !isspace((unsigned char)*args)) args++;
        if (*args) *args++ = '\0';
        args = trim(args);

        int quit = 0;
        if (*cmd == '\0') {
            // ignore blank lines
        } else if (!strcmp(cmd, "f") || !strcmp(cmd, "folder")) {
            choose_folder(args);
        } else if (!strcmp(cmd, "s") || !strcmp(cmd, "scan")) {
            scan(argv[0]);
        } else if (!strcmp(cmd, "l") || !strcmp(cmd, "list")) {
            print_header();
            print_items();
        } else if (!strcmp(cmd, "t") || !strcmp(cmd, "toggle")) {
            toggle(args);
        } else if (!strcmp(cmd, "a") || !strcmp(cmd, "all")) {
            select_all(1);
        } else if (!strcmp(cmd, "n") || !strcmp(cmd, "none")) {
            select_all(0);
        } else if (!strcmp(cmd, "r") || !strcmp(cmd, "remove")) {
            remove_selected();
        } else if (!strcmp(cmd, "h") || !strcmp(cmd, "help") || !strcmp(cmd, "?")) {
            print_help();
        } else if (!strcmp(cmd, "q") || !strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
            quit = 1;
        } else {
            printf("Unknown command '%s'. Type h for help.\n", cmd);
        }
        free(line);
        if (quit) break;
    }

    clear_items();
    free(state.root);
    return EXIT_SUCCESS;
}
