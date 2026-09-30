#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <math.h>
#include "../../include/cpu_utils.h"

#define ARGV_NUM 6
#define PROD "producer"
#define CONS "consumer"

typedef struct {
    int core_id;
    char* prog_path;
    double elapsed_time;
    pid_t pid;
    pthread_barrier_t *barrier;
    char** exec_args;
} pthr_argv;

static int cores[2] = {-1, -1};
static int prod_idx = -1;
static int cons_idx = -1;

static int validate_args(int argc, char* argv[]);
static int check_exec(char* path, char* substr);
static int check_cpu_cores(char* cores_str);
static int spanw_procs(char* argv[]);
static void* run_proc(void* arg);
void get_output_path(char* out_path, size_t size, const char* in_path);
int files_identical(const char* path1, const char* path2);
size_t read_floats(const char* path, float** data);
int compare_doubles(const void* a, const void* b);
void calculate_metrics(const char* orig_path, const char* out_path);


int main(int argc, char* argv[]) {
    if(validate_args(argc, argv) != 0) {
        fprintf(stderr, "invalid args\n");
        return 1;
    }
    long last_cpu = sysconf(_SC_NPROCESSORS_CONF) - 1;
    if (last_cpu >= 0 && pin_current_thread((int)last_cpu) != 0) {
        perror("Notice: could not pin runner to last CPU");
    }
    if(spanw_procs(argv) != 0) return 1;
    char out_path[512];
    get_output_path(out_path, sizeof(out_path), argv[1]);
    if (files_identical(argv[1], out_path)) {
        printf("\nFiles are IDENTICAL\n");
    } else {
        printf("\nFiles DIFFER\n");
        calculate_metrics(argv[1], out_path);
    }
    return 0;
}



static void* run_proc(void* arg) {
    pthr_argv* args = (pthr_argv*)arg;

    args->pid = fork();
    if (args->pid < 0) return NULL;

    if (args->pid == 0) {
        raise(SIGSTOP);
        execvp(args->exec_args[0], args->exec_args);
        perror("execvp failed");
        _exit(126);
    }

    int status;
    if (waitpid(args->pid, &status, WUNTRACED) == -1) {
        perror("waitpid WUNTRACED");
        return NULL;
    }

    pthread_barrier_wait(args->barrier);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (kill(args->pid, SIGCONT) != 0) {
        perror("kill SIGCONT");
    }

    if (waitpid(args->pid, &status, 0) == -1) {
        perror("waitpid exit");
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    args->elapsed_time = (end.tv_sec - start.tv_sec) +
                         (end.tv_nsec - start.tv_nsec) / 1000000000.0;
    printf("[Core %d] %s completed in %.6f seconds\n",
           args->core_id, args->prog_path, args->elapsed_time);
    return NULL;
}

static char** build_exec_args(int start_idx, int bin_idx, char* shm_name, char* file_path, char* core_str, char* original_argv[]) {
    int num_args = (bin_idx - start_idx) + 5;
    char** exec_args = malloc(num_args * sizeof(char*));
    int k = 0;
    for (int i = start_idx; i < bin_idx; i++) exec_args[k++] = original_argv[i];
    exec_args[k++] = original_argv[bin_idx];
    exec_args[k++] = shm_name;
    exec_args[k++] = file_path;
    exec_args[k++] = core_str;
    exec_args[k] = NULL;
    return exec_args;
}

static int spanw_procs(char* argv[]) {
    pthread_barrier_t barrier;
    if (pthread_barrier_init(&barrier, NULL, 2) != 0) return 1;

    char* shm_name = argv[cons_idx + 2];
    char* file_path = argv[1];
    char prod_core_str[16];
    char cons_core_str[16];
    snprintf(prod_core_str, sizeof(prod_core_str), "%d", cores[0]);
    snprintf(cons_core_str, sizeof(cons_core_str), "%d", cores[1]);
    char** prod_args = build_exec_args(2, prod_idx, shm_name, file_path, prod_core_str, argv);
    char** cons_args = build_exec_args(prod_idx + 1, cons_idx, shm_name, file_path, cons_core_str, argv);

    pthr_argv args1 = {cores[0], argv[prod_idx], 0.0, -1, &barrier, prod_args};
    pthr_argv args2 = {cores[1], argv[cons_idx], 0.0, -1, &barrier, cons_args};
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, run_proc, &args1) != 0) {
        pthread_barrier_destroy(&barrier);
        free(prod_args); free(cons_args);
        return 1;
    }
    if (pthread_create(&t2, NULL, run_proc, &args2) != 0) {
        pthread_cancel(t1);
        pthread_join(t1, NULL);
        pthread_barrier_destroy(&barrier);
        free(prod_args); free(cons_args);
        return 1;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_barrier_destroy(&barrier);
    free(prod_args); free(cons_args);
    printf("\n%s (Core %d) time: %.6f seconds\n", argv[prod_idx], cores[0], args1.elapsed_time);
    printf("%s (Core %d) time: %.6f seconds\n", argv[cons_idx], cores[1], args2.elapsed_time);
    return 0;
}

static int validate_args(int argc, char* argv[]) {
    if(argc < 6) return 1;

    struct stat st1;
    if(stat(argv[1], &st1) != 0 || st1.st_size == 0) {
        fprintf(stderr, "invalid file\n");
        return 1;
    }

    for (int i = 2; i < argc - 2; i++) {
        if (prod_idx == -1 && check_exec(argv[i], PROD) == 0) {
            prod_idx = i;
        } else if (prod_idx != -1 && cons_idx == -1 && check_exec(argv[i], CONS) == 0) {
            cons_idx = i;
            break;
        }
    }

    if (prod_idx == -1 || cons_idx == -1) {
        fprintf(stderr, "Could not locate valid producer or consumer binaries in arguments\n");
        return 1;
    }

    if (argc != cons_idx + 3) {
        fprintf(stderr, "Invalid trailing arguments\n");
        return 1;
    }

    if(check_cpu_cores(argv[cons_idx + 1]) != 0) return 1;
    if(strlen(argv[cons_idx + 2]) == 0) return 1;
    if (strchr(argv[cons_idx + 2], '/') != NULL) return 1;

    return 0;
}

static int check_exec(char* path, char* substr) {
    struct stat st;

    if(stat(path, &st) != 0) return 1;
    if(!S_ISREG(st.st_mode)) return 1;
    if(access(path, X_OK) != 0) return 1;

    const char* filename = strrchr(path, '/');
    if (filename != NULL) {
        filename++;
    } else {
        filename = path;
    }

    if (strstr(filename, substr) == NULL) return 1;

    return 0;
}

static int check_cpu_cores(char* cores_str) {
    char *ptr = (char*)cores_str;
    char *endptr;
    for (int i = 0; i < 2; i++) {
        if (i > 0) {
            if (*ptr != ',') return 1;
            ptr++;
        }
        long val = strtol(ptr, &endptr, 10);
        if (ptr == endptr) return 1;
        if (val < 0) return 1;
        cores[i] = (int)val;
        ptr = endptr;
    }
    if (*ptr != '\0') return 1;
    if (cores[0] == cores[1]) {
        return 1;
    }
    long max_cores = sysconf(_SC_NPROCESSORS_CONF);
    if (max_cores <= 0) return 1;

    for (int i = 0; i < 2; i++) {
        if (cores[i] >= max_cores) {
            return 1;
        }
    }
    return 0;
}

void get_output_path(char* out_path, size_t size, const char* in_path) {
    const char *name = strrchr(in_path, '/');
    name = name ? name + 1 : in_path;
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name) {
        ext = in_path + strlen(in_path);
    }
    int len = ext - in_path;
    snprintf(out_path, size, "%.*s_output%s", len, in_path, ext);
}

int files_identical(const char* path1, const char* path2) {
    FILE* f1 = fopen(path1, "rb");
    FILE* f2 = fopen(path2, "rb");
    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return 0;
    }
    char buf1[4096], buf2[4096];
    size_t r1, r2;
    int identical = 1;
    do {
        r1 = fread(buf1, 1, sizeof(buf1), f1);
        r2 = fread(buf2, 1, sizeof(buf2), f2);
        if (r1 != r2 || memcmp(buf1, buf2, r1) != 0) {
            identical = 0;
            break;
        }
    } while (r1 > 0);
    fclose(f1);
    fclose(f2);
    return identical;
}

size_t read_floats(const char* path, float** data) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    size_t capacity = 1024;
    size_t count = 0;
    *data = malloc(capacity * sizeof(float));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* end;
        float val = strtof(line, &end);
        if (end != line) {
            if (count >= capacity) {
                capacity *= 2;
                *data = realloc(*data, capacity * sizeof(float));
            }
            (*data)[count++] = val;
        }
    }
    fclose(f);

    if (count == 0) {
        f = fopen(path, "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        count = size / sizeof(float);
        *data = malloc(count * sizeof(float));
        fread(*data, sizeof(float), count, f);
        fclose(f);
    }
    return count;
}

int compare_doubles(const void* a, const void* b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

void calculate_metrics(const char* orig_path, const char* out_path) {
    float* orig_data = NULL;
    float* out_data = NULL;
    size_t orig_count = read_floats(orig_path, &orig_data);
    size_t out_count = read_floats(out_path, &out_data);

    if (orig_count == 0 || out_count == 0) {
        printf("Failed to read data for metrics\n");
        free(orig_data); free(out_data);
        return;
    }

    size_t count = orig_count < out_count ? orig_count : out_count;
    if (orig_count != out_count) {
        printf("File sizes differ\nOriginal: %zu elements, Output: %zu elements\n", orig_count, out_count);
    }

    double sum_orig = 0.0;
    double sum_diff = 0.0;
    double* abs_diffs = malloc(count * sizeof(double));

    for (size_t i = 0; i < count; i++) {
        sum_orig += orig_data[i];
        double diff = fabs((double)orig_data[i] - (double)out_data[i]);
        sum_diff += diff;
        abs_diffs[i] = diff;
    }

    double mean_orig = sum_orig / count;
    double mean_diff = sum_diff / count;

    qsort(abs_diffs, count, sizeof(double), compare_doubles);

    double median_diff;
    if (count % 2 == 0) {
        median_diff = (abs_diffs[count / 2 - 1] + abs_diffs[count / 2]) / 2.0;
    } else {
        median_diff = abs_diffs[count / 2];
    }

    printf("\nFile Comparison Metrics\n");
    printf("Elements compared: %zu\n", count);
    printf("Mean of original data:       %f\n", mean_orig);
    printf("Mean Absolute Deviation: %e\n", mean_diff);
    printf("Median Absolute Deviation: %e\n", median_diff);

    free(orig_data);
    free(out_data);
    free(abs_diffs);
}
