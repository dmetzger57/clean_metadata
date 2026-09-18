#define _XOPEN_SOURCE 700
#if defined(__APPLE__)
// Expose BSD/Darwin extensions (e.g. _SC_NPROCESSORS_ONLN) that _XOPEN_SOURCE
// would otherwise hide under strict POSIX conformance.
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_PATH 4096

// Builtin target patterns to identify and remove if config file is absent
static const char *BUILTIN_PATTERNS[] = {
    ".DS_Store",
    ".AppleDouble",
    ".Spotlight-V100",
    ".Trashes",
    ".fseventsd",
    ".DocumentRevisions-V100",
    ".TemporaryItems",
    ".VolumeIcon.icns",
    ".localized",
    "VolumeConfiguration.plist",
    ".com.apple.timemachine.donotpresent",
    ".com.apple.timemachine.supported",
    ".apdisk",
    ".MobileBackups",
    ".MobileBackups.trash",
    "Thumbs.db",
    "ehthumbs.db",
    NULL
};

// Global dynamic array for loaded target patterns (owns the strings)
static char **target_patterns = NULL;
static size_t pattern_count = 0;

// Hash set built over target_patterns for O(1) average lookups instead of
// a linear strcmp scan per directory entry. Entries borrow their string
// pointers from target_patterns, which remains the owner.
typedef struct PatternEntry {
    const char *name;
    struct PatternEntry *next;
} PatternEntry;

typedef struct {
    PatternEntry **buckets;
    size_t bucket_count;
} PatternSet;

static PatternSet pattern_set = { NULL, 0 };

// FNV-1a 64-bit
static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// Loads patterns from ${HOME}/.clean_metadata_patterns if available, otherwise loads builtins
static void load_patterns(void) {
    const char *home = getenv("HOME");
    FILE *file = NULL;

    if (home != NULL) {
        char config_path[MAX_PATH];
        snprintf(config_path, sizeof(config_path), "%s/.clean_metadata_patterns", home);
        file = fopen(config_path, "r");
    }

    if (file != NULL) {
        char line[MAX_PATH];
        size_t capacity = 10;
        target_patterns = malloc(capacity * sizeof(char *));

        while (fgets(line, sizeof(line), file) != NULL) {
            // Trim trailing newline / carriage return
            line[strcspn(line, "\r\n")] = '\0';

            // Skip empty lines or comment lines starting with '#'
            if (line[0] == '\0' || line[0] == '#') {
                continue;
            }

            if (pattern_count + 1 >= capacity) {
                capacity *= 2;
                target_patterns = realloc(target_patterns, capacity * sizeof(char *));
            }

            target_patterns[pattern_count] = strdup(line);
            pattern_count++;
        }
        fclose(file);

        if (pattern_count > 0) {
            target_patterns[pattern_count] = NULL;
            return;
        }

        // If file had no valid entries, free memory and fall back to builtins
        free(target_patterns);
        target_patterns = NULL;
    }

    // Fallback: Copy BUILTIN_PATTERNS into dynamic array
    size_t count = 0;
    while (BUILTIN_PATTERNS[count] != NULL) {
        count++;
    }

    target_patterns = malloc((count + 1) * sizeof(char *));
    for (size_t i = 0; i < count; i++) {
        target_patterns[i] = strdup(BUILTIN_PATTERNS[i]);
    }
    target_patterns[count] = NULL;
    pattern_count = count;
}

// Build the lookup hash set over the already-loaded target_patterns.
static void build_pattern_set(void) {
    size_t buckets = next_pow2(pattern_count * 2 < 8 ? 8 : pattern_count * 2);
    pattern_set.buckets = calloc(buckets, sizeof(PatternEntry *));
    pattern_set.bucket_count = buckets;

    for (size_t i = 0; i < pattern_count; i++) {
        uint64_t h = hash_str(target_patterns[i]) & (buckets - 1);
        PatternEntry *e = malloc(sizeof(PatternEntry));
        e->name = target_patterns[i];
        e->next = pattern_set.buckets[h];
        pattern_set.buckets[h] = e;
    }
}

static void free_pattern_set(void) {
    if (!pattern_set.buckets) return;
    for (size_t i = 0; i < pattern_set.bucket_count; i++) {
        PatternEntry *e = pattern_set.buckets[i];
        while (e) {
            PatternEntry *tmp = e;
            e = e->next;
            free(tmp);
        }
    }
    free(pattern_set.buckets);
    pattern_set.buckets = NULL;
    pattern_set.bucket_count = 0;
}

// Cleanup allocated target patterns
static void free_patterns(void) {
    if (!target_patterns) return;
    for (size_t i = 0; target_patterns[i] != NULL; i++) {
        free(target_patterns[i]);
    }
    free(target_patterns);
}

// Node structure for collected target paths (path is heap-allocated to its
// actual length rather than a fixed MAX_PATH buffer embedded in every node)
typedef struct Node {
    char *path;
    struct Node *next;
} Node;

// Shared thread-safe queue/list structure
typedef struct {
    Node *head;
    pthread_mutex_t lock;
    int count;
} MatchList;

static MatchList matches = { NULL, PTHREAD_MUTEX_INITIALIZER, 0 };

// Thread pool task queue
typedef struct DirectoryTask {
    char *path;
    struct DirectoryTask *next;
} DirectoryTask;

typedef struct {
    DirectoryTask *head;
    DirectoryTask *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int active_workers;
    int stop;
} TaskQueue;

static TaskQueue task_queue = { NULL, NULL, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 };

// Returns a thread count sized to the host, with a sane floor and cap.
static int get_num_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    if (n > 32) n = 32; // I/O-bound work sees diminishing returns beyond this
    return (int)n;
}

// Helper to check if a filename matches target patterns (O(1) average via hash set)
static int is_target_pattern(const char *filename) {
    uint64_t h = hash_str(filename) & (pattern_set.bucket_count - 1);
    for (PatternEntry *e = pattern_set.buckets[h]; e != NULL; e = e->next) {
        if (strcmp(filename, e->name) == 0) {
            return 1;
        }
    }
    return 0;
}

// Thread-safe addition of matched items
static void add_match(const char *path) {
    Node *node = malloc(sizeof(Node));
    if (!node) return;
    node->path = strdup(path);
    if (!node->path) { free(node); return; }

    pthread_mutex_lock(&matches.lock);
    node->next = matches.head;
    matches.head = node;
    matches.count++;
    pthread_mutex_unlock(&matches.lock);
}

// Queue management routines
static void enqueue_task(const char *path) {
    DirectoryTask *task = malloc(sizeof(DirectoryTask));
    if (!task) return;
    task->path = strdup(path);
    if (!task->path) { free(task); return; }
    task->next = NULL;

    pthread_mutex_lock(&task_queue.lock);
    if (task_queue.tail) {
        task_queue.tail->next = task;
        task_queue.tail = task;
    } else {
        task_queue.head = task;
        task_queue.tail = task;
    }
    pthread_cond_signal(&task_queue.cond);
    pthread_mutex_unlock(&task_queue.lock);
}

// Directory scanning logic executed by worker threads
static void process_directory(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        if (is_target_pattern(entry->d_name)) {
            add_match(full_path);
            // Once a directory target is matched, we do not recurse into it
            continue;
        }

        // Prefer d_type from readdir() to avoid an lstat() syscall per entry.
        // Only fall back to lstat() when the filesystem doesn't populate it
        // (some network/FUSE mounts report DT_UNKNOWN).
        int is_dir;
#if defined(DT_DIR) && defined(DT_UNKNOWN)
        if (entry->d_type == DT_DIR) {
            is_dir = 1;
        } else if (entry->d_type == DT_UNKNOWN) {
            struct stat statbuf;
            is_dir = (lstat(full_path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode));
        } else {
            is_dir = 0;
        }
#else
        struct stat statbuf;
        is_dir = (lstat(full_path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode));
#endif

        if (is_dir) {
            enqueue_task(full_path);
        }
    }
    closedir(dir);
}

// Worker thread routine
static void *worker_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&task_queue.lock);

        while (task_queue.head == NULL && !task_queue.stop) {
            if (task_queue.active_workers == 0) {
                // All directories processed and no tasks remaining
                task_queue.stop = 1;
                pthread_cond_broadcast(&task_queue.cond);
                pthread_mutex_unlock(&task_queue.lock);
                return NULL;
            }
            pthread_cond_wait(&task_queue.cond, &task_queue.lock);
        }

        if (task_queue.stop && task_queue.head == NULL) {
            pthread_mutex_unlock(&task_queue.lock);
            return NULL;
        }

        DirectoryTask *task = task_queue.head;
        task_queue.head = task->next;
        if (!task_queue.head) task_queue.tail = NULL;

        task_queue.active_workers++;
        pthread_mutex_unlock(&task_queue.lock);

        process_directory(task->path);

        pthread_mutex_lock(&task_queue.lock);
        task_queue.active_workers--;
        pthread_mutex_unlock(&task_queue.lock);

        free(task->path);
        free(task);
    }
    return NULL;
}

// Recursive deletion helper for matched files/folders
static int remove_recursive(const char *path) {
    struct stat statbuf;
    if (lstat(path, &statbuf) != 0) return -1;

    if (S_ISDIR(statbuf.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) return rmdir(path);

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char subpath[MAX_PATH];
            snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);
            remove_recursive(subpath);
        }
        closedir(dir);
        return rmdir(path);
    } else {
        return unlink(path);
    }
}

// Shared work list for parallel deletion: matched items are handed out to
// worker threads via a single shared index so large matched trees (e.g. a
// populated .Spotlight-V100 or .fseventsd) don't serialize on the main
// thread after a parallel scan.
typedef struct {
    Node **items;
    size_t count;
    size_t next;
    pthread_mutex_t lock;
} DeleteJob;

static DeleteJob delete_job = { NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER };

static void *delete_worker(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&delete_job.lock);
        if (delete_job.next >= delete_job.count) {
            pthread_mutex_unlock(&delete_job.lock);
            return NULL;
        }
        Node *node = delete_job.items[delete_job.next++];
        pthread_mutex_unlock(&delete_job.lock);

        printf("  Removing: %s\n", node->path);
        if (remove_recursive(node->path) != 0) {
            perror("  Failed to remove");
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <path-to-process>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *target_dir = argv[1];
    struct stat statbuf;
    if (stat(target_dir, &statbuf) != 0 || !S_ISDIR(statbuf.st_mode)) {
        fprintf(stderr, "Error: Directory '%s' does not exist or is not a directory.\n", target_dir);
        return EXIT_FAILURE;
    }

    // Fully buffer stdout so large match/removal listings don't trigger a
    // write() syscall per line; flushed explicitly wherever we need output
    // to be visible before blocking (e.g. the confirmation prompt).
    static char stdout_buf[1 << 16];
    setvbuf(stdout, stdout_buf, _IOFBF, sizeof(stdout_buf));

    // Load dynamic/builtin patterns and index them for O(1) lookups
    load_patterns();
    build_pattern_set();

    printf("Scanning: %s\n\n", target_dir);

    // Initial root task
    enqueue_task(target_dir);

    // Spawn worker threads sized to the host for scanning
    int num_threads = get_num_threads();
    pthread_t threads[num_threads];
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    if (matches.count == 0) {
        printf("No target housekeeping files or directories found.\n");
        free_pattern_set();
        free_patterns();
        fflush(stdout);
        return EXIT_SUCCESS;
    }

    printf("The following %d item(s) were found:\n", matches.count);
    Node *curr = matches.head;
    while (curr) {
        printf("  %s\n", curr->path);
        curr = curr->next;
    }

    // Interactive permission request
    printf("\nDelete these items? [y/N]: ");
    fflush(stdout); // ensure the prompt is visible before we block on input
    char response[10];
    if (fgets(response, sizeof(response), stdin) == NULL ||
       (response[0] != 'y' && response[0] != 'Y')) {
        printf("Operation cancelled.\n");

        // Cleanup memory
        curr = matches.head;
        while (curr) {
            Node *tmp = curr;
            curr = curr->next;
            free(tmp->path);
            free(tmp);
        }
        free_pattern_set();
        free_patterns();
        fflush(stdout);
        return EXIT_SUCCESS;
    }

    printf("\nDeleting...\n");
    fflush(stdout);

    // Fan the deletions out across worker threads instead of walking the
    // (potentially large) matched trees one at a time on the main thread.
    Node **match_array = malloc(matches.count * sizeof(Node *));
    size_t idx = 0;
    curr = matches.head;
    while (curr) {
        match_array[idx++] = curr;
        curr = curr->next;
    }

    size_t total_matches = (size_t)matches.count;
    delete_job.items = match_array;
    delete_job.count = total_matches;
    delete_job.next = 0;

    int delete_threads = get_num_threads();
    if ((size_t)delete_threads > total_matches) delete_threads = (int)total_matches;
    pthread_t dthreads[delete_threads];
    for (int i = 0; i < delete_threads; i++) {
        pthread_create(&dthreads[i], NULL, delete_worker, NULL);
    }
    for (int i = 0; i < delete_threads; i++) {
        pthread_join(dthreads[i], NULL);
    }

    for (size_t i = 0; i < total_matches; i++) {
        free(match_array[i]->path);
        free(match_array[i]);
    }
    free(match_array);

    // Touch index inhibition marker file (matching script logic)
    char marker_path[MAX_PATH];
    snprintf(marker_path, sizeof(marker_path), "%s/.metadata_never_index", target_dir);
    FILE *f = fopen(marker_path, "a");
    if (f) {
        fclose(f);
        printf("Created index inhibition marker: %s\n", marker_path);
    }

    free_pattern_set();
    free_patterns();
    printf("\nCleanup complete.\n");
    fflush(stdout);
    return EXIT_SUCCESS;
}
