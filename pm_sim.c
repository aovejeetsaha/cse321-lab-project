#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_PROCESSES 64
#define MAX_CHILDREN 64
#define MAX_LINE 256

typedef enum {
    PROC_RUNNING,
    PROC_WAITING,
    PROC_ZOMBIE
} ProcessState;

typedef struct {
    int used;
    int pid;
    int ppid;
    ProcessState state;
    int exit_status;
    int children[MAX_CHILDREN];
    int child_count;
    pthread_cond_t wait_cond;   /* parent sleep on this */
} PCB;

typedef struct {
    int thread_id;
    char filename[256];
} WorkerArg;

static PCB process_table[MAX_PROCESSES];
static int next_pid = 2;                 /* pid1 is init */
static int active_workers = 0;
static int monitor_pending = 0;

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t monitor_cond = PTHREAD_COND_INITIALIZER;

static FILE *snapshot_fp = NULL;

/* utility */

static const char *state_to_string(ProcessState state) {
    switch (state) {
        case PROC_RUNNING: return "RUNNING";
        case PROC_WAITING: return "WAITING";
        case PROC_ZOMBIE:  return "ZOMBIE";
        default:           return "UNKNOWN";
    }
}

static int find_index_by_pid(int pid) {
    int i;
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].used && process_table[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void) {
    int i;
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (!process_table[i].used) {
            return i;
        }
    }
    return -1;
}

static void notify_monitor(void) {
    monitor_pending = 1;
    pthread_cond_signal(&monitor_cond);
}

static void add_child_to_parent_nolock(int parent_pid, int child_pid) {
    int pidx = find_index_by_pid(parent_pid);
    if (pidx == -1) return;

    if (process_table[pidx].child_count < MAX_CHILDREN) {
        process_table[pidx].children[process_table[pidx].child_count++] = child_pid;
    }
}

static void remove_child_from_parent_nolock(int parent_pid, int child_pid) {
    int pidx = find_index_by_pid(parent_pid);
    int i, j;

    if (pidx == -1) return;

    for (i = 0; i < process_table[pidx].child_count; i++) {
        if (process_table[pidx].children[i] == child_pid) {
            for (j = i; j < process_table[pidx].child_count - 1; j++) {
                process_table[pidx].children[j] = process_table[pidx].children[j + 1];
            }
            process_table[pidx].children[process_table[pidx].child_count - 1] = 0;
            process_table[pidx].child_count--;
            break;
        }
    }
}

static int parent_has_any_child_nolock(int parent_pid) {
    int pidx = find_index_by_pid(parent_pid);
    if (pidx == -1) return 0;
    return process_table[pidx].child_count > 0;
}

static int parent_has_specific_child_nolock(int parent_pid, int child_pid) {
    int pidx = find_index_by_pid(parent_pid);
    int i;

    if (pidx == -1) return 0;

    for (i = 0; i < process_table[pidx].child_count; i++) {
        if (process_table[pidx].children[i] == child_pid) {
            return 1;
        }
    }
    return 0;
}

static int reap_child_nolock(int parent_pid, int child_pid, int *status_out) {
    int cidx = find_index_by_pid(child_pid);
    int i;

    if (cidx == -1) return 0;
    if (process_table[cidx].ppid != parent_pid) return 0;
    if (process_table[cidx].state != PROC_ZOMBIE) return 0;

    if (status_out != NULL) {
        *status_out = process_table[cidx].exit_status;
    }

    remove_child_from_parent_nolock(parent_pid, child_pid);

    process_table[cidx].used = 0;
    process_table[cidx].pid = 0;
    process_table[cidx].ppid = 0;
    process_table[cidx].state = PROC_RUNNING;
    process_table[cidx].exit_status = -1;
    process_table[cidx].child_count = 0;
    for (i = 0; i < MAX_CHILDREN; i++) {
        process_table[cidx].children[i] = 0;
    }

    notify_monitor();
    return 1;
}

/* snapshot printing */

static void pm_ps_locked(FILE *out) {
    int i;

    fprintf(out, "PID PPID STATE EXIT_STATUS\n");
    fprintf(out, "----------------------------------------------\n");

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].used) {
            if (process_table[i].state == PROC_ZOMBIE) {
                fprintf(out, "%d %d %s %d\n",
                        process_table[i].pid,
                        process_table[i].ppid,
                        state_to_string(process_table[i].state),
                        process_table[i].exit_status);
            } else {
                fprintf(out, "%d %d %s -\n",
                        process_table[i].pid,
                        process_table[i].ppid,
                        state_to_string(process_table[i].state));
            }
        }
    }
    fprintf(out, "\n");
    fflush(out);
}

static void write_command_line_locked(int tid, const char *text) {
    fprintf(snapshot_fp, "Thread %d %s\n", tid, text);
    fflush(snapshot_fp);
}

/* required operations */

int pm_fork(int parent_pid) {
    int pidx, slot, child_pid;

    pthread_mutex_lock(&table_mutex);

    pidx = find_index_by_pid(parent_pid);
    if (pidx == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    if (process_table[pidx].state == PROC_ZOMBIE) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    slot = find_free_slot();
    if (slot == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    child_pid = next_pid++;

    process_table[slot].used = 1;
    process_table[slot].pid = child_pid;
    process_table[slot].ppid = parent_pid;
    process_table[slot].state = PROC_RUNNING;
    process_table[slot].exit_status = -1;
    process_table[slot].child_count = 0;
    pthread_cond_init(&process_table[slot].wait_cond, NULL);
    memset(process_table[slot].children, 0, sizeof(process_table[slot].children));

    add_child_to_parent_nolock(parent_pid, child_pid);

    notify_monitor();
    pthread_mutex_unlock(&table_mutex);
    return child_pid;
}

void pm_exit(int pid, int status) {
    int idx, pidx;

    pthread_mutex_lock(&table_mutex);

    idx = find_index_by_pid(pid);
    if (idx == -1) {
        pthread_mutex_unlock(&table_mutex);
        return;
    }

    if (process_table[idx].state == PROC_ZOMBIE) {
        pthread_mutex_unlock(&table_mutex);
        return;
    }

    process_table[idx].state = PROC_ZOMBIE;
    process_table[idx].exit_status = status;

    pidx = find_index_by_pid(process_table[idx].ppid);
    if (pidx != -1) {
        pthread_cond_broadcast(&process_table[pidx].wait_cond);
    }

    notify_monitor();
    pthread_mutex_unlock(&table_mutex);
}

void pm_kill(int pid) {
    pm_exit(pid, -9);
}

int pm_wait(int parent_pid, int child_pid, int *status_out) {
    int pidx;
    int i;

    pthread_mutex_lock(&table_mutex);

    pidx = find_index_by_pid(parent_pid);
    if (pidx == -1) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    if (child_pid == -1) {
        if (!parent_has_any_child_nolock(parent_pid)) {
            pthread_mutex_unlock(&table_mutex);
            return -1;
        }

        while (1) {
            for (i = 0; i < process_table[pidx].child_count; i++) {
                int cpid = process_table[pidx].children[i];
                if (reap_child_nolock(parent_pid, cpid, status_out)) {
                    process_table[pidx].state = PROC_RUNNING;
                    pthread_mutex_unlock(&table_mutex);
                    return cpid;
                }
            }

            if (!parent_has_any_child_nolock(parent_pid)) {
                process_table[pidx].state = PROC_RUNNING;
                pthread_mutex_unlock(&table_mutex);
                return -1;
            }

            process_table[pidx].state = PROC_WAITING;
            notify_monitor();
            pthread_cond_wait(&process_table[pidx].wait_cond, &table_mutex);
        }
    }

    if (!parent_has_specific_child_nolock(parent_pid, child_pid)) {
        pthread_mutex_unlock(&table_mutex);
        return -1;
    }

    while (1) {
        if (reap_child_nolock(parent_pid, child_pid, status_out)) {
            process_table[pidx].state = PROC_RUNNING;
            pthread_mutex_unlock(&table_mutex);
            return child_pid;
        }

        if (!parent_has_specific_child_nolock(parent_pid, child_pid)) {
            process_table[pidx].state = PROC_RUNNING;
            pthread_mutex_unlock(&table_mutex);
            return -1;
        }

        process_table[pidx].state = PROC_WAITING;
        notify_monitor();
        pthread_cond_wait(&process_table[pidx].wait_cond, &table_mutex);
    }
}

/* threads */

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

void *worker_thread_func(void *arg) {
    WorkerArg *info = (WorkerArg *)arg;
    FILE *fp;
    char line[MAX_LINE];

    fp = fopen(info->filename, "r");
    if (fp == NULL) {
        pthread_mutex_lock(&table_mutex);
        write_command_line_locked(info->thread_id, "could not open script file");
        active_workers--;
        pthread_cond_signal(&monitor_cond);
        pthread_mutex_unlock(&table_mutex);
        return NULL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char cmd[32];
        trim_newline(line);

        if (line[0] == '\0') continue;
        if (sscanf(line, "%31s", cmd) != 1) continue;

        if (strcmp(cmd, "fork") == 0) {
            int parent_pid;
            if (sscanf(line, "fork %d", &parent_pid) == 1) {
                char msg[128];
                snprintf(msg, sizeof(msg), "calls pm_fork %d", parent_pid);
                pthread_mutex_lock(&table_mutex);
                write_command_line_locked(info->thread_id, msg);
                pthread_mutex_unlock(&table_mutex);
                pm_fork(parent_pid);
            }
        } else if (strcmp(cmd, "exit") == 0) {
            int pid, status;
            if (sscanf(line, "exit %d %d", &pid, &status) == 2) {
                char msg[128];
                snprintf(msg, sizeof(msg), "calls pm_exit %d %d", pid, status);
                pthread_mutex_lock(&table_mutex);
                write_command_line_locked(info->thread_id, msg);
                pthread_mutex_unlock(&table_mutex);
                pm_exit(pid, status);
            }
        } else if (strcmp(cmd, "wait") == 0) {
            int parent_pid, child_pid;
            int status;
            if (sscanf(line, "wait %d %d", &parent_pid, &child_pid) == 2) {
                char msg[128];
                snprintf(msg, sizeof(msg), "calls pm_wait %d %d", parent_pid, child_pid);
                pthread_mutex_lock(&table_mutex);
                write_command_line_locked(info->thread_id, msg);
                pthread_mutex_unlock(&table_mutex);
                pm_wait(parent_pid, child_pid, &status);
            }
        } else if (strcmp(cmd, "kill") == 0) {
            int pid;
            if (sscanf(line, "kill %d", &pid) == 1) {
                char msg[128];
                snprintf(msg, sizeof(msg), "calls pm_kill %d", pid);
                pthread_mutex_lock(&table_mutex);
                write_command_line_locked(info->thread_id, msg);
                pthread_mutex_unlock(&table_mutex);
                pm_kill(pid);
            }
        } else if (strcmp(cmd, "sleep") == 0) {
            int ms;
            if (sscanf(line, "sleep %d", &ms) == 1) {
                usleep(ms * 1000);
            }
        }
    }

    fclose(fp);

    pthread_mutex_lock(&table_mutex);
    active_workers--;
    pthread_cond_signal(&monitor_cond);
    pthread_mutex_unlock(&table_mutex);
    return NULL;
}

void *monitor_thread_func(void *arg) {
    (void)arg;

    pthread_mutex_lock(&table_mutex);

    fprintf(snapshot_fp, "Initial Process Table\n");
    pm_ps_locked(snapshot_fp);

    while (1) {
        while (!monitor_pending && active_workers > 0) {
            pthread_cond_wait(&monitor_cond, &table_mutex);
        }

        if (monitor_pending) {
            pm_ps_locked(snapshot_fp);
            monitor_pending = 0;
        }

        if (active_workers == 0 && !monitor_pending) {
            break;
        }
    }

    pthread_mutex_unlock(&table_mutex);
    return NULL;
}

/* init + main */

static void initialize_process_table(void) {
    int i, j;

    for (i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].used = 0;
        process_table[i].pid = 0;
        process_table[i].ppid = 0;
        process_table[i].state = PROC_RUNNING;
        process_table[i].exit_status = -1;
        process_table[i].child_count = 0;
        for (j = 0; j < MAX_CHILDREN; j++) {
            process_table[i].children[j] = 0;
        }
        pthread_cond_init(&process_table[i].wait_cond, NULL);
    }

    process_table[0].used = 1;
    process_table[0].pid = 1;
    process_table[0].ppid = 0;
    process_table[0].state = PROC_RUNNING;
    process_table[0].exit_status = -1;
    process_table[0].child_count = 0;
}

int main(int argc, char *argv[]) {
    int i;
    pthread_t monitor_thread;
    pthread_t *worker_threads;
    WorkerArg *worker_args;

    if (argc < 2) {
        printf("Usage: %s thread0.txt thread1.txt ...\n", argv[0]);
        return 1;
    }

    snapshot_fp = fopen("snapshots.txt", "w");
    if (snapshot_fp == NULL) {
        printf("Could not create snapshots.txt\n");
        return 1;
    }

    initialize_process_table();
    active_workers = argc - 1;

    worker_threads = (pthread_t *)malloc((argc - 1) * sizeof(pthread_t));
    worker_args = (WorkerArg *)malloc((argc - 1) * sizeof(WorkerArg));

    if (worker_threads == NULL || worker_args == NULL) {
        printf("Memory allocation failed\n");
        fclose(snapshot_fp);
        return 1;
    }

    pthread_create(&monitor_thread, NULL, monitor_thread_func, NULL);

    for (i = 1; i < argc; i++) {
        worker_args[i - 1].thread_id = i - 1;
        strncpy(worker_args[i - 1].filename, argv[i], sizeof(worker_args[i - 1].filename) - 1);
        worker_args[i - 1].filename[sizeof(worker_args[i - 1].filename) - 1] = '\0';
        pthread_create(&worker_threads[i - 1], NULL, worker_thread_func, &worker_args[i - 1]);
    }

    for (i = 0; i < argc - 1; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    pthread_mutex_lock(&table_mutex);
    pthread_cond_signal(&monitor_cond);
    pthread_mutex_unlock(&table_mutex);

    pthread_join(monitor_thread, NULL);

    for (i = 0; i < MAX_PROCESSES; i++) {
        pthread_cond_destroy(&process_table[i].wait_cond);
    }

    free(worker_threads);
    free(worker_args);
    fclose(snapshot_fp);
    return 0;
}
