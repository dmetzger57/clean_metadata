#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_PATH 4096

// Target patterns to identify and remove
static const char *TARGET_PATTERNS[] = {
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

// Node structure for collected target paths
typedef struct Node {
    char path[MAX_PATH];
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
    char path[MAX_PATH];
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

// Helper to check if a filename matches target patterns
static int is_target_pattern(const char *filename) {
    for (int i = 0; TARGET_PATTERNS[i] != NULL; i++) {
        if (strcmp(filename, TARGET_PATTERNS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// Thread-safe addition of matched items
static void add_match(const char *path) {
    Node *node = malloc(sizeof(Node));
    if (!node) return;
    snprintf(node->path, sizeof(node->path), "%s", path);

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
    snprintf(task->path, sizeof(task->path), "%s", path);
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

        struct stat statbuf;
        if (lstat(full_path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
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

    printf("Scanning: %s\n\n", target_dir);

    // Initial root task
    enqueue_task(target_dir);

    // Spawn 8 worker threads for scanning
    int num_threads = 8;
    pthread_t threads[num_threads];
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, worker_thread, NULL);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    if (matches.count == 0) {
        printf("No target housekeeping files or directories found.\n");
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
    char response[10];
    if (fgets(response, sizeof(response), stdin) == NULL || 
       (response[0] != 'y' && response[0] != 'Y')) {
        printf("Operation cancelled.\n");
        
        // Cleanup memory
        curr = matches.head;
        while (curr) {
            Node *tmp = curr;
            curr = curr->next;
            free(tmp);
        }
        return EXIT_SUCCESS;
    }

    printf("\nDeleting...\n");
    curr = matches.head;
    while (curr) {
        printf("  Removing: %s\n", curr->path);
        if (remove_recursive(curr->path) != 0) {
            perror("  Failed to remove");
        }
        Node *tmp = curr;
        curr = curr->next;
        free(tmp);
    }

    // Touch index inhibition marker file (matching script logic)
    char marker_path[MAX_PATH];
    snprintf(marker_path, sizeof(marker_path), "%s/.metadata_never_index", target_dir);
    FILE *f = fopen(marker_path, "a");
    if (f) {
        fclose(f);
        printf("Created index inhibition marker: %s\n", marker_path);
    }

    printf("\nCleanup complete.\n");
    return EXIT_SUCCESS;
}
